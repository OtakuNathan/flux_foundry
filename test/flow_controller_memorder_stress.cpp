#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

#include "flow/flow.h"

using namespace flux_foundry;

namespace {

using err_t = std::exception_ptr;
using out_t = result_t<int, err_t>;

struct run_cfg {
    int inline_true_iters{3000};
    int noninline_inline_iters{3000};
    int queued_after_unlock_iters{3000};
    int cancel_race_iters{2000};
};

static run_cfg g_cfg;

void check(bool cond, const char* name, int& failed) {
    if (cond) {
        std::printf("[OK] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++failed;
    }
}

struct observer {
    std::atomic<int> done{0};
    std::atomic<int> has_value{-1};
    std::atomic<int> value{0};
    std::exception_ptr err;
};

struct int_receiver {
    using value_type = out_t;
    observer* st;

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

bool wait_done(observer& st, int timeout_ms) {
    auto begin = std::chrono::steady_clock::now();
    while (st.done.load(std::memory_order_acquire) == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(20));
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
        if (msg == "flow soft-canceled") return err_kind::cancel_soft;
        if (msg == "flow hard-canceled") return err_kind::cancel_hard;
        if (msg == "failed to submit async operation") return err_kind::submit_fail;
        return err_kind::other;
    } catch (...) {
        return err_kind::other;
    }
}

// Not recognized as flow_impl::inline_executor by the trait, but executes inline.
// This deterministically drives make_async_next_step(..., false_type) "success-under-lock"
// path (the CAS around flow_runner.h:482) when awaitable resumes synchronously in submit().
struct inline_like_executor {
    void dispatch(task_wrapper_sbo t) noexcept {
        t();
    }
};

// Queued executor with a gate so tests can force continuation execution to happen only after
// runner submit path returns (i.e., after the guard unlocks). This deterministically drives
// make_async_next_step(..., false_type) CAS-fail -> reset_cancel_handler() path.
class gated_queue_executor {
public:
    gated_queue_executor()
        : worker_([this]() noexcept { run(); }) {
    }

    ~gated_queue_executor() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stopping_ = true;
            gate_open_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void pause() noexcept {
        std::lock_guard<std::mutex> lk(mu_);
        gate_open_ = false;
    }

    void open_gate() noexcept {
        {
            std::lock_guard<std::mutex> lk(mu_);
            gate_open_ = true;
        }
        cv_.notify_all();
    }

    void close_gate() noexcept {
        std::lock_guard<std::mutex> lk(mu_);
        gate_open_ = false;
    }

    void wait_until_queued_at_least(size_t n, int timeout_ms) {
        auto begin = std::chrono::steady_clock::now();
        for (;;) {
            if (queued_count_.load(std::memory_order_acquire) >= static_cast<int>(n)) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(20));
            auto elapsed = std::chrono::steady_clock::now() - begin;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > timeout_ms) {
                std::printf("[FAIL] gated_queue_executor queue wait timeout\n");
                std::abort();
            }
        }
    }

    void wait_drained(int timeout_ms) {
        auto begin = std::chrono::steady_clock::now();
        for (;;) {
            if (inflight_.load(std::memory_order_acquire) == 0) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(20));
            auto elapsed = std::chrono::steady_clock::now() - begin;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > timeout_ms) {
                std::printf("[FAIL] gated_queue_executor drain timeout\n");
                std::abort();
            }
        }
    }

    void dispatch(task_wrapper_sbo t) noexcept {
        {
            std::lock_guard<std::mutex> lk(mu_);
            q_.emplace_back(std::move(t));
            inflight_.fetch_add(1, std::memory_order_relaxed);
            queued_count_.fetch_add(1, std::memory_order_release);
        }
        cv_.notify_one();
    }

private:
    void run() noexcept {
        for (;;) {
            task_wrapper_sbo task;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this]() noexcept {
                    return stopping_ || (gate_open_ && !q_.empty());
                });

                if (stopping_ && q_.empty()) {
                    return;
                }

                if (!gate_open_ || q_.empty()) {
                    continue;
                }

                task = std::move(q_.front());
                q_.pop_front();
            }

            task();
            inflight_.fetch_sub(1, std::memory_order_release);
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<task_wrapper_sbo> q_;
    bool stopping_{false};
    bool gate_open_{true};
    std::atomic<int> inflight_{0};
    std::atomic<int> queued_count_{0};
    std::thread worker_;
};

struct immediate_plus_one_awaitable final : awaitable_base<immediate_plus_one_awaitable, int, err_t> {
    using async_result_type = out_t;
    int v;

    explicit immediate_plus_one_awaitable(async_result_type&& in) noexcept
        : v(in.has_value() ? in.value() : 0) {
    }

    int submit() noexcept {
        this->resume(async_result_type(value_tag, v + 1));
        return 0;
    }

    void cancel() noexcept {
    }
};

struct test_stat {
    int iterations{0};
    int executed{0};
    int timeout{0};
    int duplicate_callback{0};
    int value_count{0};
    int error_count{0};
    int cancel_soft{0};
    int cancel_hard{0};
};

template <typename RunnerFn>
test_stat run_loop(int iters, RunnerFn&& fn) {
    test_stat st;
    st.iterations = iters;
    for (int i = 0; i < iters; ++i) {
        auto r = fn(i);
        if (!wait_done(*r.first, 1000)) {
            ++st.timeout;
            continue;
        }
        ++st.executed;
        int done = r.first->done.load(std::memory_order_acquire);
        if (done != 1) {
            ++st.duplicate_callback;
        }
        if (r.first->has_value.load(std::memory_order_acquire) == 1) {
            ++st.value_count;
        } else {
            ++st.error_count;
            auto k = classify_error(r.first->err);
            if (k == err_kind::cancel_soft) ++st.cancel_soft;
            if (k == err_kind::cancel_hard) ++st.cancel_hard;
        }
    }
    return st;
}

void print_stat(const char* name, const test_stat& s) {
    std::printf("[%s]\n", name);
    std::printf("  iterations         : %d\n", s.iterations);
    std::printf("  executed           : %d\n", s.executed);
    std::printf("  timeout            : %d\n", s.timeout);
    std::printf("  duplicate_callback : %d\n", s.duplicate_callback);
    std::printf("  value_count        : %d\n", s.value_count);
    std::printf("  error_count        : %d\n", s.error_count);
    std::printf("  cancel_soft        : %d\n", s.cancel_soft);
    std::printf("  cancel_hard        : %d\n", s.cancel_hard);
}

int stress_inline_true_success_under_lock() {
    // By construction:
    // - normal awaitable path installs cancel handler + lock
    // - awaitable resumes synchronously in submit()
    // - default await<> dispatcher is framework inline executor
    // => make_async_next_step(..., token, true_type) CAS success path is taken.
    using bp_t = decltype(make_blueprint<int>()
                          | await<immediate_plus_one_awaitable>()
                          | end());
    auto bp = make_blueprint<int>()
        | await<immediate_plus_one_awaitable>()
        | end();
    auto bp_ptr = make_lite_ptr<bp_t>(std::move(bp));

    auto stat = run_loop(g_cfg.inline_true_iters, [&](int i) {
        auto st = std::make_shared<observer>();
        auto runner = make_runner(bp_ptr, int_receiver{st.get()});
        runner(i);
        return std::make_pair(st, 0);
    });

    print_stat("memorder.inline_true.success_under_lock", stat);
    int failed = 0;
    check(stat.timeout == 0, "inline_true no timeout", failed);
    check(stat.duplicate_callback == 0, "inline_true no duplicate callback", failed);
    check(stat.value_count == g_cfg.inline_true_iters, "inline_true all value", failed);
    return failed;
}

int stress_noninline_inline_dispatch_success_under_lock() {
    // By construction:
    // - dispatcher type is NOT recognized as inline by trait (false_type path)
    // - but dispatch executes inline immediately
    // - awaitable resumes synchronously in submit()
    // => make_async_next_step(..., token, false_type) inner CAS success path is taken.
    inline_like_executor ex;
    using bp_t = decltype(make_blueprint<int>()
                          | await<immediate_plus_one_awaitable>(&ex)
                          | end());
    auto bp = make_blueprint<int>()
        | await<immediate_plus_one_awaitable>(&ex)
        | end();
    auto bp_ptr = make_lite_ptr<bp_t>(std::move(bp));

    auto stat = run_loop(g_cfg.noninline_inline_iters, [&](int i) {
        auto st = std::make_shared<observer>();
        auto runner = make_runner(bp_ptr, int_receiver{st.get()});
        runner(i);
        return std::make_pair(st, 0);
    });

    print_stat("memorder.noninline_inline_dispatch.success_under_lock", stat);
    int failed = 0;
    check(stat.timeout == 0, "noninline-inline no timeout", failed);
    check(stat.duplicate_callback == 0, "noninline-inline no duplicate callback", failed);
    check(stat.value_count == g_cfg.noninline_inline_iters, "noninline-inline all value", failed);
    return failed;
}

int stress_noninline_queued_dispatch_after_unlock() {
    // By construction:
    // - false_type path (custom executor)
    // - executor gate is closed during runner submit path
    // - awaitable resumes synchronously and queues continuation
    // - runner returns and unlocks before queued task runs
    // => make_async_next_step(..., token, false_type) inner CAS fails and reset_cancel_handler()
    //    runs through its lock-acquire path.
    gated_queue_executor ex;
    ex.pause();

    using bp_t = decltype(make_blueprint<int>()
                          | await<immediate_plus_one_awaitable>(&ex)
                          | end());
    auto bp = make_blueprint<int>()
        | await<immediate_plus_one_awaitable>(&ex)
        | end();
    auto bp_ptr = make_lite_ptr<bp_t>(std::move(bp));

    test_stat stat;
    stat.iterations = g_cfg.queued_after_unlock_iters;

    for (int i = 0; i < stat.iterations; ++i) {
        auto st = std::make_shared<observer>();
        auto runner = make_runner(bp_ptr, int_receiver{st.get()});
        runner(i);

        ex.wait_until_queued_at_least(static_cast<size_t>(i + 1), 1000);
        ex.open_gate();
        bool ok = wait_done(*st, 1000);
        ex.close_gate();

        if (!ok) {
            ++stat.timeout;
            continue;
        }
        ++stat.executed;
        int done = st->done.load(std::memory_order_acquire);
        if (done != 1) {
            ++stat.duplicate_callback;
        }
        if (st->has_value.load(std::memory_order_acquire) == 1) {
            ++stat.value_count;
        } else {
            ++stat.error_count;
        }
    }

    ex.open_gate();
    ex.wait_drained(1000);
    print_stat("memorder.noninline_queued.after_unlock", stat);
    int failed = 0;
    check(stat.timeout == 0, "noninline-queued no timeout", failed);
    check(stat.duplicate_callback == 0, "noninline-queued no duplicate callback", failed);
    check(stat.value_count == g_cfg.queued_after_unlock_iters, "noninline-queued all value", failed);
    return failed;
}

int stress_noninline_queued_cancel_race() {
    // Covers the updated cancel() acquire/relaxed CAS path together with the same false_type
    // queued continuation path above.
    gated_queue_executor ex;
    ex.pause();

    using bp_t = decltype(make_blueprint<int>()
                          | await<immediate_plus_one_awaitable>(&ex)
                          | end());
    auto bp = make_blueprint<int>()
        | await<immediate_plus_one_awaitable>(&ex)
        | end();
    auto bp_ptr = make_lite_ptr<bp_t>(std::move(bp));

    test_stat stat;
    stat.iterations = g_cfg.cancel_race_iters;

    for (int i = 0; i < stat.iterations; ++i) {
        auto st = std::make_shared<observer>();
        auto ctrl = make_lite_ptr<flow_controller>();
        flow_runner<bp_t, int_receiver> runner(bp_ptr, ctrl, int_receiver{st.get()});
        runner(i);

        ex.wait_until_queued_at_least(static_cast<size_t>(i + 1), 1000);

        std::thread t1([c = ctrl]() noexcept { c->cancel(false); });
        std::thread t2([c = ctrl]() noexcept { c->cancel(true); });
        t1.join();
        t2.join();

        ex.open_gate();
        bool ok = wait_done(*st, 1000);
        ex.close_gate();

        if (!ok) {
            ++stat.timeout;
            continue;
        }
        ++stat.executed;
        int done = st->done.load(std::memory_order_acquire);
        if (done != 1) {
            ++stat.duplicate_callback;
        }
        if (st->has_value.load(std::memory_order_acquire) == 1) {
            ++stat.value_count;
        } else {
            ++stat.error_count;
            auto k = classify_error(st->err);
            if (k == err_kind::cancel_soft) ++stat.cancel_soft;
            if (k == err_kind::cancel_hard) ++stat.cancel_hard;
        }
    }

    ex.open_gate();
    ex.wait_drained(1000);
    print_stat("memorder.noninline_queued.cancel_race", stat);
    int failed = 0;
    check(stat.timeout == 0, "cancel_race no timeout", failed);
    check(stat.duplicate_callback == 0, "cancel_race no duplicate callback", failed);
    check(stat.executed == stat.iterations, "cancel_race all executed", failed);
    check(stat.value_count + stat.error_count == stat.iterations, "cancel_race result accounting", failed);
    return failed;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 5) {
        g_cfg.inline_true_iters = std::atoi(argv[1]);
        g_cfg.noninline_inline_iters = std::atoi(argv[2]);
        g_cfg.queued_after_unlock_iters = std::atoi(argv[3]);
        g_cfg.cancel_race_iters = std::atoi(argv[4]);
    }
    int failed = 0;
    failed += stress_inline_true_success_under_lock();
    failed += stress_noninline_inline_dispatch_success_under_lock();
    failed += stress_noninline_queued_dispatch_after_unlock();
    failed += stress_noninline_queued_cancel_race();

    if (failed == 0) {
        std::printf("[PASS] flow_controller memorder stress passed\n");
        return 0;
    }
    std::printf("[FAIL] flow_controller memorder stress failed=%d\n", failed);
    return 1;
}
