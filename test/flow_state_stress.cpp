#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "flow/flow.h"

using namespace flux_foundry;

namespace {
using err_t = std::exception_ptr;
using out_t = result_t<int, err_t>;

static std::atomic<unsigned long long> g_rng{0x9E3779B97F4A7C15ull};
static std::atomic<int> g_backend_inflight{0};

unsigned fast_rand_u32() noexcept {
    auto x = g_rng.fetch_add(0x9E3779B97F4A7C15ull, std::memory_order_relaxed);
    x ^= (x >> 12);
    x ^= (x << 25);
    x ^= (x >> 27);
    return static_cast<unsigned>((x * 2685821657736338717ull) >> 32);
}

void spin_for_us(unsigned us) {
    auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    while (std::chrono::steady_clock::now() < until) {
    }
}

void random_sleep_us(unsigned max_us) {
    if (max_us == 0) {
        return;
    }
    auto us = fast_rand_u32() % max_us;
    spin_for_us(us);
}

class work_group {
public:
    explicit work_group(size_t nthreads)
        : stopping_(false) {
        workers_.reserve(nthreads);
        for (size_t i = 0; i < nthreads; ++i) {
            workers_.emplace_back([this]() { this->run(); });
        }
    }

    ~work_group() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) {
                w.join();
            }
        }
    }

    void post(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lk(m_);
            q_.push_back(std::move(fn));
        }
        cv_.notify_one();
    }

private:
    void run() {
        for (;;) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this]() { return stopping_ || !q_.empty(); });
                if (stopping_ && q_.empty()) {
                    return;
                }
                fn = std::move(q_.front());
                q_.pop_front();
            }
            fn();
        }
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    std::vector<std::thread> workers_;
    bool stopping_;
};

work_group& backend_pool() {
    static work_group pool(8);
    return pool;
}

struct inline_executor {
    void dispatch(task_wrapper_sbo t) noexcept {
        t();
    }
};

static inline_executor g_inline_executor;

struct delayed_plus_one_awaitable final : awaitable_base<delayed_plus_one_awaitable, int, err_t> {
    using async_result_type = out_t;

    int input;

    explicit delayed_plus_one_awaitable(async_result_type&& in) noexcept
        : input(in.has_value() ? in.value() : 0) {
    }

    int submit() noexcept {
        this->retain();
        g_backend_inflight.fetch_add(1, std::memory_order_relaxed);

        int v = input;
        unsigned delay = 10 + (fast_rand_u32() % 150);
        backend_pool().post([self = this, v, delay]() noexcept {
            spin_for_us(delay);
            self->resume(async_result_type(value_tag, v + 1));
            self->release();
            g_backend_inflight.fetch_sub(1, std::memory_order_release);
        });
        return 0;
    }

    void cancel() noexcept {
    }
};

struct always_fail_submit_awaitable final : awaitable_base<always_fail_submit_awaitable, int, err_t> {
    using async_result_type = out_t;

    explicit always_fail_submit_awaitable(async_result_type&&) noexcept {
    }

    int submit() noexcept {
        return -1;
    }

    void cancel() noexcept {
    }
};

struct receiver_state {
    std::atomic<int> done{0};
    std::atomic<int> has_value{-1};
    std::atomic<int> value{0};
    std::exception_ptr err;
};

struct int_receiver {
    using value_type = out_t;

    std::shared_ptr<receiver_state> st;

    void emplace(value_type&& r) noexcept {
        if (r.has_value()) {
            st->value.store(r.value(), std::memory_order_relaxed);
            st->has_value.store(1, std::memory_order_relaxed);
        } else {
            st->err = r.error();
            st->has_value.store(0, std::memory_order_relaxed);
        }
        st->done.fetch_add(1, std::memory_order_release);
    }
};

bool wait_done(const std::shared_ptr<receiver_state>& st, int timeout_ms) {
    auto begin = std::chrono::steady_clock::now();
    while (st->done.load(std::memory_order_acquire) == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        auto elapsed = std::chrono::steady_clock::now() - begin;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > timeout_ms) {
            return false;
        }
    }
    return true;
}

bool wait_backend_drained(int timeout_ms) {
    auto begin = std::chrono::steady_clock::now();
    while (g_backend_inflight.load(std::memory_order_acquire) != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        auto elapsed = std::chrono::steady_clock::now() - begin;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > timeout_ms) {
            return false;
        }
    }
    return true;
}

enum class err_kind {
    none,
    cancel_soft,
    cancel_hard,
    submit_fail,
    other,
};

err_kind classify_error(const std::exception_ptr& ep) {
    if (!ep) {
        return err_kind::none;
    }

    try {
        std::rethrow_exception(ep);
    } catch (const std::logic_error& e) {
        const std::string msg = e.what();
        if (msg == "flow soft-canceled") {
            return err_kind::cancel_soft;
        }
        if (msg == "flow hard-canceled") {
            return err_kind::cancel_hard;
        }
        if (msg == "failed to submit async operation") {
            return err_kind::submit_fail;
        }
        return err_kind::other;
    } catch (...) {
        return err_kind::other;
    }
}

struct test_stat {
    int iterations = 0;
    int executed = 0;
    int timeout = 0;
    int duplicate_callback = 0;
    int value_count = 0;
    int error_count = 0;
    int cancel_soft = 0;
    int cancel_hard = 0;
    int submit_fail = 0;
    int other_error = 0;
    long long elapsed_ms = 0;
};

void print_stat(const char* name, const test_stat& s) {
    std::printf("[%s]\n", name);
    std::printf("  iterations         : %d\n", s.iterations);
    std::printf("  executed           : %d\n", s.executed);
    std::printf("  elapsed_ms         : %lld\n", s.elapsed_ms);
    std::printf("  timeout            : %d\n", s.timeout);
    std::printf("  duplicate_callback : %d\n", s.duplicate_callback);
    std::printf("  value_count        : %d\n", s.value_count);
    std::printf("  error_count        : %d\n", s.error_count);
    std::printf("  cancel_soft        : %d\n", s.cancel_soft);
    std::printf("  cancel_hard        : %d\n", s.cancel_hard);
    std::printf("  submit_fail        : %d\n", s.submit_fail);
    std::printf("  other_error        : %d\n", s.other_error);
}

void consume_outcome(const std::shared_ptr<receiver_state>& st, test_stat& s) {
    int done = st->done.load(std::memory_order_acquire);
    if (done != 1) {
        ++s.duplicate_callback;
    }

    int hv = st->has_value.load(std::memory_order_relaxed);
    if (hv == 1) {
        ++s.value_count;
        return;
    }

    ++s.error_count;
    switch (classify_error(st->err)) {
        case err_kind::cancel_soft:
            ++s.cancel_soft;
            break;
        case err_kind::cancel_hard:
            ++s.cancel_hard;
            break;
        case err_kind::submit_fail:
            ++s.submit_fail;
            break;
        case err_kind::other:
            ++s.other_error;
            break;
        case err_kind::none:
            ++s.other_error;
            break;
    }
}

test_stat stress_async_cancel_race() {
    constexpr int kIters = 5000;
    constexpr int kTimeoutAbort = 50;

    auto bp = make_blueprint<int>()
        | await<delayed_plus_one_awaitable>(&g_inline_executor)
        | end();
    using bp_t = decltype(bp);
    auto bp_ptr = make_lite_ptr<bp_t>(std::move(bp));

    test_stat s;
    s.iterations = kIters;
    auto begin = std::chrono::steady_clock::now();

    for (int i = 0; i < kIters; ++i) {
        if ((i + 1) % 500 == 0) {
            std::printf("[async cancel race] progress %d/%d\n", i + 1, kIters);
            std::fflush(stdout);
        }

        auto ctrl = make_lite_ptr<flow_controller>();
        auto st = std::make_shared<receiver_state>();

        flow_runner<bp_t, int_receiver> runner(bp_ptr, ctrl, int_receiver{st});
        runner(i);

        for (int j = 0; j < 8; ++j) {
            random_sleep_us(60);
            ctrl->cancel((fast_rand_u32() & 1u) != 0u);
        }

        ++s.executed;
        if (!wait_done(st, 120)) {
            ++s.timeout;
            if (s.timeout >= kTimeoutAbort) {
                break;
            }
            continue;
        }

        consume_outcome(st, s);
    }

    s.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin).count();
    return s;
}

test_stat stress_submit_fail_path() {
    constexpr int kIters = 5000;
    constexpr int kTimeoutAbort = 50;

    auto bp = make_blueprint<int>()
        | await<always_fail_submit_awaitable>(&g_inline_executor)
        | end();
    using bp_t = decltype(bp);
    auto bp_ptr = make_lite_ptr<bp_t>(std::move(bp));

    test_stat s;
    s.iterations = kIters;
    auto begin = std::chrono::steady_clock::now();

    for (int i = 0; i < kIters; ++i) {
        if ((i + 1) % 500 == 0) {
            std::printf("[submit fail] progress %d/%d\n", i + 1, kIters);
            std::fflush(stdout);
        }

        auto ctrl = make_lite_ptr<flow_controller>();
        auto st = std::make_shared<receiver_state>();

        flow_runner<bp_t, int_receiver> runner(bp_ptr, ctrl, int_receiver{st});
        runner(i);

        ++s.executed;
        if (!wait_done(st, 120)) {
            ++s.timeout;
            if (s.timeout >= kTimeoutAbort) {
                break;
            }
            continue;
        }

        consume_outcome(st, s);
    }

    s.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin).count();
    return s;
}

template <typename BP1, typename BP2>
auto make_when_all_stress_bp(std::false_type, lite_ptr<BP1> p1, lite_ptr<BP2> p2) {
    return await_when_all(
        &g_inline_executor,
        [](int a, int b) noexcept {
            return out_t(value_tag, a + b);
        },
        [](flow_async_agg_err_t e) noexcept {
            return out_t(error_tag, std::move(e));
        },
        p1,
        p2)
        | end();
}

template <typename BP1, typename BP2>
auto make_when_all_stress_bp(std::true_type, lite_ptr<BP1> p1, lite_ptr<BP2> p2) {
    return await_when_all_fast(
        &g_inline_executor,
        [](int a, int b) noexcept {
            return out_t(value_tag, a + b);
        },
        [](flow_async_agg_err_t e) noexcept {
            return out_t(error_tag, std::move(e));
        },
        p1,
        p2)
        | end();
}

template <typename BP1, typename BP2>
auto make_when_any_stress_bp(std::false_type, lite_ptr<BP1> p1, lite_ptr<BP2> p2) {
    return await_when_any(
        &g_inline_executor,
        [](int x) noexcept {
            return out_t(value_tag, x);
        },
        [](flow_async_agg_err_t e) noexcept {
            return out_t(error_tag, std::move(e));
        },
        p1,
        p2)
        | end();
}

template <typename BP1, typename BP2>
auto make_when_any_stress_bp(std::true_type, lite_ptr<BP1> p1, lite_ptr<BP2> p2) {
    return await_when_any_fast(
        &g_inline_executor,
        [](int x) noexcept {
            return out_t(value_tag, x);
        },
        [](flow_async_agg_err_t e) noexcept {
            return out_t(error_tag, std::move(e));
        },
        p1,
        p2)
        | end();
}

template <bool RunnerFast, bool AggFast>
test_stat stress_when_all_matrix_case(const char* tag) {
    constexpr int kIters = 1000;
    constexpr int kTimeoutAbort = 40;

    auto leaf1 = make_blueprint<int>()
        | await<delayed_plus_one_awaitable>(&g_inline_executor)
        | end();
    auto leaf2 = make_blueprint<int>()
        | await<delayed_plus_one_awaitable>(&g_inline_executor)
        | end();

    auto p1 = make_lite_ptr<decltype(leaf1)>(std::move(leaf1));
    auto p2 = make_lite_ptr<decltype(leaf2)>(std::move(leaf2));

    auto bp = make_when_all_stress_bp(std::integral_constant<bool, AggFast>{}, p1, p2);
    using bp_t = decltype(bp);
    auto bp_ptr = make_lite_ptr<bp_t>(std::move(bp));

    test_stat s;
    s.iterations = kIters;
    auto begin = std::chrono::steady_clock::now();

    for (int i = 0; i < kIters; ++i) {
        if ((i + 1) % 200 == 0) {
            std::printf("%s progress %d/%d\n", tag, i + 1, kIters);
            std::fflush(stdout);
        }

        auto st = std::make_shared<receiver_state>();
        if (RunnerFast) {
            auto runner = make_fast_runner(bp_ptr, int_receiver{st});
            runner(make_flat_storage(i, i + 1));
        } else {
            auto ctrl = make_lite_ptr<flow_controller>();
            flow_runner<bp_t, int_receiver> runner(bp_ptr, ctrl, int_receiver{st});
            runner(make_flat_storage(i, i + 1));

            if (!AggFast) {
                random_sleep_us(120);
                ctrl->cancel((fast_rand_u32() & 1u) != 0u);
            }
        }

        ++s.executed;
        if (!wait_done(st, 180)) {
            ++s.timeout;
            if (s.timeout >= kTimeoutAbort) {
                break;
            }
            continue;
        }

        consume_outcome(st, s);
    }

    s.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin).count();
    return s;
}

template <bool RunnerFast, bool AggFast>
test_stat stress_when_any_matrix_case(const char* tag) {
    constexpr int kIters = 1000;
    constexpr int kTimeoutAbort = 40;

    auto leaf1 = make_blueprint<int>()
        | await<delayed_plus_one_awaitable>(&g_inline_executor)
        | end();
    auto leaf2 = make_blueprint<int>()
        | await<delayed_plus_one_awaitable>(&g_inline_executor)
        | end();

    auto p1 = make_lite_ptr<decltype(leaf1)>(std::move(leaf1));
    auto p2 = make_lite_ptr<decltype(leaf2)>(std::move(leaf2));

    auto bp = make_when_any_stress_bp(std::integral_constant<bool, AggFast>{}, p1, p2);
    using bp_t = decltype(bp);
    auto bp_ptr = make_lite_ptr<bp_t>(std::move(bp));

    test_stat s;
    s.iterations = kIters;
    auto begin = std::chrono::steady_clock::now();

    for (int i = 0; i < kIters; ++i) {
        if ((i + 1) % 200 == 0) {
            std::printf("%s progress %d/%d\n", tag, i + 1, kIters);
            std::fflush(stdout);
        }

        auto st = std::make_shared<receiver_state>();
        if (RunnerFast) {
            auto runner = make_fast_runner(bp_ptr, int_receiver{st});
            runner(make_flat_storage(i, i + 1));
        } else {
            auto ctrl = make_lite_ptr<flow_controller>();
            flow_runner<bp_t, int_receiver> runner(bp_ptr, ctrl, int_receiver{st});
            runner(make_flat_storage(i, i + 1));

            if (!AggFast) {
                random_sleep_us(120);
                ctrl->cancel((fast_rand_u32() & 1u) != 0u);
            }
        }

        ++s.executed;
        if (!wait_done(st, 180)) {
            ++s.timeout;
            if (s.timeout >= kTimeoutAbort) {
                break;
            }
            continue;
        }

        consume_outcome(st, s);
    }

    s.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin).count();
    return s;
}

int score_failures(const test_stat& s, bool allow_submit_fail_only = false) {
    int fail = 0;
    fail += s.timeout;
    fail += s.duplicate_callback;

    if (allow_submit_fail_only) {
        fail += s.other_error;
        fail += s.cancel_soft;
        fail += s.cancel_hard;
        fail += (s.submit_fail == s.executed ? 0 : 1);
    }

    return fail;
}

} // namespace

int main() {
    int failed = 0;

    auto whole_begin = std::chrono::steady_clock::now();

    auto t1 = stress_async_cancel_race();
    auto t2 = stress_submit_fail_path();
    auto t3 = stress_when_all_matrix_case<false, false>("[when_all normal/cancel]");
    auto t4 = stress_when_all_matrix_case<false, true>("[when_all normal/no_cancel]");
    auto t5 = stress_when_all_matrix_case<true, false>("[when_all fast/cancel]");
    auto t6 = stress_when_all_matrix_case<true, true>("[when_all fast/no_cancel]");
    auto t7 = stress_when_any_matrix_case<false, false>("[when_any normal/cancel]");
    auto t8 = stress_when_any_matrix_case<false, true>("[when_any normal/no_cancel]");
    auto t9 = stress_when_any_matrix_case<true, false>("[when_any fast/cancel]");
    auto t10 = stress_when_any_matrix_case<true, true>("[when_any fast/no_cancel]");

    print_stat("async cancel race", t1);
    print_stat("submit fail path", t2);
    print_stat("when_all normal/cancel", t3);
    print_stat("when_all normal/no_cancel", t4);
    print_stat("when_all fast/cancel", t5);
    print_stat("when_all fast/no_cancel", t6);
    print_stat("when_any normal/cancel", t7);
    print_stat("when_any normal/no_cancel", t8);
    print_stat("when_any fast/cancel", t9);
    print_stat("when_any fast/no_cancel", t10);

    failed += score_failures(t1);
    failed += score_failures(t2, true);
    failed += score_failures(t3);
    failed += score_failures(t4);
    failed += score_failures(t5);
    failed += score_failures(t6);
    failed += score_failures(t7);
    failed += score_failures(t8);
    failed += score_failures(t9);
    failed += score_failures(t10);

    if (!wait_backend_drained(15000)) {
        std::printf("[FAIL] backend tasks not drained in time\n");
        failed += 1;
    }

    auto whole_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - whole_begin).count();
    std::printf("[TOTAL] elapsed_ms=%lld\n", whole_ms);

    if (failed == 0) {
        std::printf("[PASS] state-machine stress passed\n");
        return 0;
    }

    std::printf("[FAIL] state-machine stress found %d issues\n", failed);
    return 1;
}
