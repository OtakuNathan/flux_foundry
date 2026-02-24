#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
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

static std::atomic<unsigned long long> g_rng{0xD1B54A32D192ED03ull};
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

void random_spin_us(unsigned max_us) {
    if (max_us == 0) {
        return;
    }
    spin_for_us(fast_rand_u32() % max_us);
}

class work_group {
public:
    explicit work_group(size_t nthreads)
        : stopping_(false), pending_(0) {
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
        pending_.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(m_);
            q_.push_back(std::move(fn));
        }
        cv_.notify_one();
    }

    bool wait_idle(int timeout_ms) const {
        auto begin = std::chrono::steady_clock::now();
        while (pending_.load(std::memory_order_acquire) != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            auto elapsed = std::chrono::steady_clock::now() - begin;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > timeout_ms) {
                return false;
            }
        }
        return true;
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

            try {
                fn();
            } catch (...) {
            }

            pending_.fetch_sub(1, std::memory_order_release);
        }
    }

private:
    mutable std::mutex m_;
    mutable std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    std::vector<std::thread> workers_;
    bool stopping_;
    std::atomic<int> pending_;
};

work_group& backend_pool() {
    static work_group pool(8);
    return pool;
}

work_group& cancel_pool() {
    static work_group pool(4);
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

        const int v = input;
        const unsigned delay = 10 + (fast_rand_u32() % 120);
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

bool wait_counter_at_least(const std::atomic<int>& x, int target, int timeout_ms) {
    auto begin = std::chrono::steady_clock::now();
    while (x.load(std::memory_order_acquire) < target) {
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
    int cancel_wait_timeout = 0;
    int duplicate_callback = 0;
    int value_count = 0;
    int error_count = 0;
    int cancel_soft = 0;
    int cancel_hard = 0;
    int submit_fail = 0;
    int other_error = 0;
    long long elapsed_ms = 0;
};

void merge_stat(test_stat& dst, const test_stat& src) {
    dst.iterations += src.iterations;
    dst.executed += src.executed;
    dst.timeout += src.timeout;
    dst.cancel_wait_timeout += src.cancel_wait_timeout;
    dst.duplicate_callback += src.duplicate_callback;
    dst.value_count += src.value_count;
    dst.error_count += src.error_count;
    dst.cancel_soft += src.cancel_soft;
    dst.cancel_hard += src.cancel_hard;
    dst.submit_fail += src.submit_fail;
    dst.other_error += src.other_error;
}

void consume_outcome(const std::shared_ptr<receiver_state>& st, test_stat& s) {
    const int done = st->done.load(std::memory_order_acquire);
    if (done != 1) {
        ++s.duplicate_callback;
    }

    const int hv = st->has_value.load(std::memory_order_relaxed);
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

struct config_t {
    int workers = 8;
    int iters_per_worker = 500;
    int cancel_threads = 4;
    int cancel_calls_per_thread = 4;
    int timeout_ms = 200;
};

int arg_or_default(char** argv, int argc, int index, int fallback) {
    if (index >= argc) {
        return fallback;
    }
    const int v = std::atoi(argv[index]);
    return v > 0 ? v : fallback;
}

template <typename BpPtr>
test_stat worker_run(const BpPtr& bp_ptr, int worker_id, const config_t& cfg,
    std::atomic<int>& ready, std::atomic<int>& go)
{
    using bp_t = typename BpPtr::element_type;

    test_stat s;
    s.iterations = cfg.iters_per_worker;

    ready.fetch_add(1, std::memory_order_acq_rel);
    while (go.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }

    auto begin = std::chrono::steady_clock::now();

    for (int i = 0; i < cfg.iters_per_worker; ++i) {
        if ((i + 1) % 100 == 0) {
            std::printf("[worker %d] progress %d/%d\n", worker_id, i + 1, cfg.iters_per_worker);
            std::fflush(stdout);
        }

        auto ctrl = make_lite_ptr<flow_controller>();
        auto st = std::make_shared<receiver_state>();
        auto cancel_done = std::make_shared<std::atomic<int>>(0);

        flow_runner<bp_t, int_receiver> runner(bp_ptr, ctrl, int_receiver{st});
        runner(worker_id * 1000000 + i);

        for (int t = 0; t < cfg.cancel_threads; ++t) {
            auto ctrl_copy = ctrl;
            auto cancel_done_copy = cancel_done;
            cancel_pool().post([ctrl_copy, cancel_done_copy, cfg]() noexcept {
                for (int k = 0; k < cfg.cancel_calls_per_thread; ++k) {
                    random_spin_us(60);
                    ctrl_copy->cancel((fast_rand_u32() & 1u) != 0u);
                }
                cancel_done_copy->fetch_add(1, std::memory_order_release);
            });
        }

        // Also issue a few local cancels to diversify interleavings.
        for (int k = 0; k < 2; ++k) {
            random_spin_us(40);
            ctrl->cancel((fast_rand_u32() & 1u) != 0u);
        }

        ++s.executed;
        if (!wait_done(st, cfg.timeout_ms)) {
            ++s.timeout;
        } else {
            consume_outcome(st, s);
        }

        if (!wait_counter_at_least(*cancel_done, cfg.cancel_threads, cfg.timeout_ms)) {
            ++s.cancel_wait_timeout;
        }
    }

    s.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin).count();
    return s;
}

void print_stat(const test_stat& s, const config_t& cfg, long long total_ms) {
    std::printf("[flow multiflow cancel stress]\n");
    std::printf("  workers                : %d\n", cfg.workers);
    std::printf("  iters_per_worker       : %d\n", cfg.iters_per_worker);
    std::printf("  cancel_threads         : %d\n", cfg.cancel_threads);
    std::printf("  cancel_calls_per_thr   : %d\n", cfg.cancel_calls_per_thread);
    std::printf("  iterations             : %d\n", s.iterations);
    std::printf("  executed               : %d\n", s.executed);
    std::printf("  total_elapsed_ms       : %lld\n", total_ms);
    std::printf("  timeout                : %d\n", s.timeout);
    std::printf("  cancel_wait_timeout    : %d\n", s.cancel_wait_timeout);
    std::printf("  duplicate_callback     : %d\n", s.duplicate_callback);
    std::printf("  value_count            : %d\n", s.value_count);
    std::printf("  error_count            : %d\n", s.error_count);
    std::printf("  cancel_soft            : %d\n", s.cancel_soft);
    std::printf("  cancel_hard            : %d\n", s.cancel_hard);
    std::printf("  submit_fail            : %d\n", s.submit_fail);
    std::printf("  other_error            : %d\n", s.other_error);
}

} // namespace

int main(int argc, char** argv) {
    config_t cfg;
    cfg.workers = arg_or_default(argv, argc, 1, cfg.workers);
    cfg.iters_per_worker = arg_or_default(argv, argc, 2, cfg.iters_per_worker);
    cfg.cancel_threads = arg_or_default(argv, argc, 3, cfg.cancel_threads);
    cfg.cancel_calls_per_thread = arg_or_default(argv, argc, 4, cfg.cancel_calls_per_thread);
    cfg.timeout_ms = arg_or_default(argv, argc, 5, cfg.timeout_ms);

    auto bp = make_blueprint<int>()
        | await<delayed_plus_one_awaitable>(&g_inline_executor)
        | end();
    using bp_t = decltype(bp);
    auto bp_ptr = make_lite_ptr<bp_t>(std::move(bp));

    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(cfg.workers));
    std::vector<test_stat> results(static_cast<size_t>(cfg.workers));

    std::atomic<int> ready{0};
    std::atomic<int> go{0};

    auto whole_begin = std::chrono::steady_clock::now();

    for (int w = 0; w < cfg.workers; ++w) {
        workers.emplace_back([&, w]() {
            results[static_cast<size_t>(w)] = worker_run(bp_ptr, w, cfg, ready, go);
        });
    }

    while (ready.load(std::memory_order_acquire) != cfg.workers) {
        std::this_thread::yield();
    }
    go.store(1, std::memory_order_release);

    for (auto& t : workers) {
        if (t.joinable()) {
            t.join();
        }
    }

    const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - whole_begin).count();

    test_stat sum;
    for (const auto& r : results) {
        merge_stat(sum, r);
    }
    print_stat(sum, cfg, total_ms);

    int failed = 0;
    failed += sum.timeout;
    failed += sum.cancel_wait_timeout;
    failed += sum.duplicate_callback;
    failed += sum.submit_fail;
    failed += sum.other_error;

    if (!wait_backend_drained(15000)) {
        std::printf("[FAIL] backend tasks not drained in time\n");
        ++failed;
    }
    if (!backend_pool().wait_idle(15000)) {
        std::printf("[FAIL] backend queue not idle in time\n");
        ++failed;
    }
    if (!cancel_pool().wait_idle(15000)) {
        std::printf("[FAIL] cancel queue not idle in time\n");
        ++failed;
    }

    if (failed == 0) {
        std::printf("[PASS] multiflow cancel stress passed\n");
        return 0;
    }

    std::printf("[FAIL] multiflow cancel stress found %d issues\n", failed);
    return 1;
}
