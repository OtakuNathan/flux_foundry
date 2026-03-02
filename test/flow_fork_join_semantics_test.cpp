#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "flow/flow.h"
#include "flow/flow_fork_receiver_tmp.h"

using namespace flux_foundry;

namespace {
using err_t = std::exception_ptr;
using out_t = result_t<int, err_t>;

struct inline_executor {
    void dispatch(task_wrapper_sbo t) noexcept {
        t();
    }
};

struct run_state {
    std::atomic<int> done{0};
    std::atomic<int> has_value{-1};
    std::atomic<int> value{0};
    std::exception_ptr err;
};

struct out_receiver {
    using value_type = out_t;

    std::shared_ptr<run_state> st;

    void emplace(value_type&& r) noexcept {
        if (r.has_value()) {
            st->value.store(r.value(), std::memory_order_relaxed);
            st->has_value.store(1, std::memory_order_relaxed);
        } else {
            st->err = r.error();
            st->has_value.store(0, std::memory_order_relaxed);
        }
        st->done.store(1, std::memory_order_release);
    }
};

void check(bool cond, const char* name, int& failed) {
    if (cond) {
        std::printf("[OK] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++failed;
    }
}

bool wait_done(const std::shared_ptr<run_state>& st, int timeout_ms) {
    const auto begin = std::chrono::steady_clock::now();
    while (st->done.load(std::memory_order_acquire) == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        const auto elapsed = std::chrono::steady_clock::now() - begin;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > timeout_ms) {
            return false;
        }
    }
    return true;
}

bool has_logic_error_message(const std::exception_ptr& ep, const char* expected) {
    if (!ep) {
        return false;
    }

    try {
        std::rethrow_exception(ep);
    } catch (const std::logic_error& e) {
        return std::string(e.what()) == expected;
    } catch (...) {
        return false;
    }
}

auto make_left_leaf_bp() {
    return make_blueprint<int>()
        | transform([](int x) noexcept { return x + 1; })
        | end();
}

auto make_right_leaf_bp() {
    return make_blueprint<int>()
        | transform([](int x) noexcept { return x * 2; })
        | end();
}

inline_executor g_inline_executor;
std::atomic<int> g_async_inflight{0};

bool wait_async_drained(int timeout_ms) {
    const auto begin = std::chrono::steady_clock::now();
    while (g_async_inflight.load(std::memory_order_acquire) != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto elapsed = std::chrono::steady_clock::now() - begin;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > timeout_ms) {
            return false;
        }
    }
    return true;
}

struct delayed_plus_one_awaitable final : awaitable_base<delayed_plus_one_awaitable, int, err_t> {
    using async_result_type = out_t;

    int input;

    explicit delayed_plus_one_awaitable(async_result_type&& in) noexcept
        : input(in.has_value() ? in.value() : 0) {
    }

    int submit() noexcept {
        this->retain();
        g_async_inflight.fetch_add(1, std::memory_order_relaxed);

        const int v = input;
        std::thread([self = this, v]() noexcept {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            self->resume(async_result_type(value_tag, v + 1));
            self->release();
            g_async_inflight.fetch_sub(1, std::memory_order_release);
        }).detach();
        return 0;
    }

    void cancel() noexcept {
    }
};

template <typename FromBP, typename LeftBP, typename RightBP>
struct normal_join_receiver final : fork_receiver<normal_join_receiver<FromBP, LeftBP, RightBP>, FromBP, LeftBP, RightBP> {
    using value_type = typename FromBP::O_t;

    lite_ptr<LeftBP> left_bp;
    lite_ptr<RightBP> right_bp;
    std::shared_ptr<run_state> st;

    normal_join_receiver(lite_ptr<LeftBP> left, lite_ptr<RightBP> right, std::shared_ptr<run_state> st_)
        noexcept
        : left_bp(std::move(left)), right_bp(std::move(right)), st(std::move(st_)) {
    }

    void forward(value_type&& in) noexcept {
        if (!in.has_value()) {
            st->err = in.error();
            st->has_value.store(0, std::memory_order_relaxed);
            st->done.store(1, std::memory_order_release);
            return;
        }

        auto join_bp = await_when_all(
            &g_inline_executor,
            [](int a, int b) noexcept {
                return out_t(value_tag, a + b);
            },
            [](flow_async_agg_err_t e) noexcept {
                return out_t(error_tag, std::move(e));
            },
            left_bp,
            right_bp)
            | end();

        auto join_bp_ptr = make_lite_ptr<decltype(join_bp)>(std::move(join_bp));
        auto join_runner = make_runner(join_bp_ptr, out_receiver{st});
        join_runner(in.value(), in.value());
    }
};

template <typename FromBP, typename LeftBP, typename RightBP>
struct fast_join_receiver final : fork_receiver<fast_join_receiver<FromBP, LeftBP, RightBP>, FromBP, LeftBP, RightBP> {
    using value_type = typename FromBP::O_t;

    lite_ptr<LeftBP> left_bp;
    lite_ptr<RightBP> right_bp;
    std::shared_ptr<run_state> st;

    fast_join_receiver(lite_ptr<LeftBP> left, lite_ptr<RightBP> right, std::shared_ptr<run_state> st_)
        noexcept
        : left_bp(std::move(left)), right_bp(std::move(right)), st(std::move(st_)) {
    }

    void forward(value_type&& in) noexcept {
        if (!in.has_value()) {
            st->err = in.error();
            st->has_value.store(0, std::memory_order_relaxed);
            st->done.store(1, std::memory_order_release);
            return;
        }

        auto join_bp = await_when_all(
            &g_inline_executor,
            [](int a, int b) noexcept {
                return out_t(value_tag, a + b);
            },
            [](flow_async_agg_err_t e) noexcept {
                return out_t(error_tag, std::move(e));
            },
            left_bp,
            right_bp)
            | end();

        auto join_bp_ptr = make_lite_ptr<decltype(join_bp)>(std::move(join_bp));
        auto join_runner = make_fast_runner(join_bp_ptr, out_receiver{st});
        join_runner(in.value(), in.value());
    }
};

template <typename FromBP, typename LeftBP, typename RightBP>
struct canceling_normal_join_receiver final :
    fork_receiver<canceling_normal_join_receiver<FromBP, LeftBP, RightBP>, FromBP, LeftBP, RightBP> {
    using value_type = typename FromBP::O_t;

    lite_ptr<LeftBP> left_bp;
    lite_ptr<RightBP> right_bp;
    std::shared_ptr<run_state> st;
    bool force_cancel = true;

    canceling_normal_join_receiver(lite_ptr<LeftBP> left, lite_ptr<RightBP> right,
        std::shared_ptr<run_state> st_, bool force)
        noexcept
        : left_bp(std::move(left)), right_bp(std::move(right)), st(std::move(st_)), force_cancel(force) {
    }

    void forward(value_type&& in) noexcept {
        if (!in.has_value()) {
            st->err = in.error();
            st->has_value.store(0, std::memory_order_relaxed);
            st->done.store(1, std::memory_order_release);
            return;
        }

        auto join_bp = await_when_all(
            &g_inline_executor,
            [](int a, int b) noexcept {
                return out_t(value_tag, a + b);
            },
            [](flow_async_agg_err_t e) noexcept {
                return out_t(error_tag, std::move(e));
            },
            left_bp,
            right_bp)
            | end();

        auto join_bp_ptr = make_lite_ptr<decltype(join_bp)>(std::move(join_bp));
        auto join_runner = make_runner(join_bp_ptr, out_receiver{st});

        join_runner(in.value(), in.value());
        auto ctrl = join_runner.get_controller();
        if (ctrl) {
            ctrl->cancel(force_cancel);
        }
    }
};

int test_fork_join_normal_runner() {
    int failed = 0;

    auto upstream_bp = make_blueprint<int>()
        | transform([](int x) noexcept { return x + 3; })
        | end();
    auto left_bp = make_left_leaf_bp();
    auto right_bp = make_right_leaf_bp();

    using upstream_bp_t = decltype(upstream_bp);
    using left_bp_t = decltype(left_bp);
    using right_bp_t = decltype(right_bp);
    using receiver_t = normal_join_receiver<upstream_bp_t, left_bp_t, right_bp_t>;

    auto upstream_bp_ptr = make_lite_ptr<upstream_bp_t>(std::move(upstream_bp));
    auto left_bp_ptr = make_lite_ptr<left_bp_t>(std::move(left_bp));
    auto right_bp_ptr = make_lite_ptr<right_bp_t>(std::move(right_bp));
    auto st = std::make_shared<run_state>();

    auto runner = make_runner(upstream_bp_ptr, receiver_t(left_bp_ptr, right_bp_ptr, st));
    runner(5);

    check(wait_done(st, 1000), "fork->join(normal runner) wait done", failed);
    check(st->has_value.load(std::memory_order_acquire) == 1, "fork->join(normal runner) has value", failed);
    check(st->value.load(std::memory_order_relaxed) == 25, "fork->join(normal runner) value == 25", failed);

    return failed;
}

int test_fork_join_fast_runner() {
    int failed = 0;

    auto upstream_bp = make_blueprint<int>()
        | transform([](int x) noexcept { return x + 3; })
        | end();
    auto left_bp = make_left_leaf_bp();
    auto right_bp = make_right_leaf_bp();

    using upstream_bp_t = decltype(upstream_bp);
    using left_bp_t = decltype(left_bp);
    using right_bp_t = decltype(right_bp);
    using receiver_t = fast_join_receiver<upstream_bp_t, left_bp_t, right_bp_t>;

    auto upstream_bp_ptr = make_lite_ptr<upstream_bp_t>(std::move(upstream_bp));
    auto left_bp_ptr = make_lite_ptr<left_bp_t>(std::move(left_bp));
    auto right_bp_ptr = make_lite_ptr<right_bp_t>(std::move(right_bp));
    auto st = std::make_shared<run_state>();

    auto runner = make_runner(upstream_bp_ptr, receiver_t(left_bp_ptr, right_bp_ptr, st));
    runner(5);

    check(wait_done(st, 1000), "fork->join(fast runner) wait done", failed);
    check(st->has_value.load(std::memory_order_acquire) == 1, "fork->join(fast runner) has value", failed);
    check(st->value.load(std::memory_order_relaxed) == 25, "fork->join(fast runner) value == 25", failed);

    return failed;
}

int test_fork_join_normal_runner_cancel() {
    int failed = 0;

    auto upstream_bp = make_blueprint<int>() | end();
    auto left_bp = make_blueprint<int>() | await<delayed_plus_one_awaitable>(&g_inline_executor) | end();
    auto right_bp = make_blueprint<int>() | await<delayed_plus_one_awaitable>(&g_inline_executor) | end();

    using upstream_bp_t = decltype(upstream_bp);
    using left_bp_t = decltype(left_bp);
    using right_bp_t = decltype(right_bp);
    using receiver_t = canceling_normal_join_receiver<upstream_bp_t, left_bp_t, right_bp_t>;

    auto upstream_bp_ptr = make_lite_ptr<upstream_bp_t>(std::move(upstream_bp));
    auto left_bp_ptr = make_lite_ptr<left_bp_t>(std::move(left_bp));
    auto right_bp_ptr = make_lite_ptr<right_bp_t>(std::move(right_bp));
    auto st = std::make_shared<run_state>();

    auto runner = make_runner(upstream_bp_ptr, receiver_t(left_bp_ptr, right_bp_ptr, st, true));
    runner(7);

    check(wait_done(st, 2000), "fork->join(normal cancel) wait done", failed);
    check(st->has_value.load(std::memory_order_acquire) == 0, "fork->join(normal cancel) has error", failed);
    check(has_logic_error_message(st->err, "flow hard-canceled"),
        "fork->join(normal cancel) error == flow hard-canceled", failed);
    check(wait_async_drained(2000), "fork->join(normal cancel) backend drained", failed);

    return failed;
}

int test_fork_join_normal_runner_soft_cancel() {
    int failed = 0;

    auto upstream_bp = make_blueprint<int>() | end();
    auto left_bp = make_blueprint<int>() | await<delayed_plus_one_awaitable>(&g_inline_executor) | end();
    auto right_bp = make_blueprint<int>() | await<delayed_plus_one_awaitable>(&g_inline_executor) | end();

    using upstream_bp_t = decltype(upstream_bp);
    using left_bp_t = decltype(left_bp);
    using right_bp_t = decltype(right_bp);
    using receiver_t = canceling_normal_join_receiver<upstream_bp_t, left_bp_t, right_bp_t>;

    auto upstream_bp_ptr = make_lite_ptr<upstream_bp_t>(std::move(upstream_bp));
    auto left_bp_ptr = make_lite_ptr<left_bp_t>(std::move(left_bp));
    auto right_bp_ptr = make_lite_ptr<right_bp_t>(std::move(right_bp));
    auto st = std::make_shared<run_state>();

    auto runner = make_runner(upstream_bp_ptr, receiver_t(left_bp_ptr, right_bp_ptr, st, false));
    runner(7);

    check(wait_done(st, 2000), "fork->join(normal soft-cancel) wait done", failed);
    check(st->has_value.load(std::memory_order_acquire) == 0, "fork->join(normal soft-cancel) has error", failed);
    check(has_logic_error_message(st->err, "flow soft-canceled"),
        "fork->join(normal soft-cancel) error == flow soft-canceled", failed);
    check(wait_async_drained(2000), "fork->join(normal soft-cancel) backend drained", failed);

    return failed;
}

int test_fork_join_upstream_error_passthrough() {
    int failed = 0;

    auto upstream_bp = make_blueprint<int>()
        | then([](out_t&&) noexcept -> out_t {
            return out_t(error_tag, std::make_exception_ptr(std::logic_error("upstream-fail")));
        })
        | end();
    auto left_bp = make_left_leaf_bp();
    auto right_bp = make_right_leaf_bp();

    using upstream_bp_t = decltype(upstream_bp);
    using left_bp_t = decltype(left_bp);
    using right_bp_t = decltype(right_bp);
    using receiver_t = normal_join_receiver<upstream_bp_t, left_bp_t, right_bp_t>;

    auto upstream_bp_ptr = make_lite_ptr<upstream_bp_t>(std::move(upstream_bp));
    auto left_bp_ptr = make_lite_ptr<left_bp_t>(std::move(left_bp));
    auto right_bp_ptr = make_lite_ptr<right_bp_t>(std::move(right_bp));
    auto st = std::make_shared<run_state>();

    auto runner = make_runner(upstream_bp_ptr, receiver_t(left_bp_ptr, right_bp_ptr, st));
    runner(5);

    check(wait_done(st, 1000), "fork->join(upstream error) wait done", failed);
    check(st->has_value.load(std::memory_order_acquire) == 0, "fork->join(upstream error) has error", failed);
    check(has_logic_error_message(st->err, "upstream-fail"),
        "fork->join(upstream error) passthrough message", failed);

    return failed;
}

int test_fork_join_submit_fail() {
    int failed = 0;

    auto upstream_bp = make_blueprint<int>() | end();
    auto left_bp = make_left_leaf_bp();
    auto right_bp = make_right_leaf_bp();

    using upstream_bp_t = decltype(upstream_bp);
    using left_bp_t = decltype(left_bp);
    using right_bp_t = decltype(right_bp);
    using receiver_t = normal_join_receiver<upstream_bp_t, left_bp_t, right_bp_t>;

    auto upstream_bp_ptr = make_lite_ptr<upstream_bp_t>(std::move(upstream_bp));
    auto left_bp_ptr = make_lite_ptr<left_bp_t>(std::move(left_bp));
    lite_ptr<right_bp_t> null_right_bp;
    auto st = std::make_shared<run_state>();

    auto runner = make_runner(upstream_bp_ptr, receiver_t(left_bp_ptr, null_right_bp, st));
    runner(5);

    check(wait_done(st, 1000), "fork->join(submit-fail) wait done", failed);
    check(st->has_value.load(std::memory_order_acquire) == 0, "fork->join(submit-fail) has error", failed);
    check(has_logic_error_message(st->err, "failed to submit async operation"),
        "fork->join(submit-fail) message", failed);

    return failed;
}
} // namespace

int main() {
    int failed = 0;
    failed += test_fork_join_normal_runner();
    failed += test_fork_join_fast_runner();
    failed += test_fork_join_normal_runner_cancel();
    failed += test_fork_join_normal_runner_soft_cancel();
    failed += test_fork_join_upstream_error_passthrough();
    failed += test_fork_join_submit_fail();

    if (failed == 0) {
        std::printf("[PASS] fork/join semantics test passed\n");
        return 0;
    }

    std::printf("[FAIL] fork/join semantics test failed with %d failures\n", failed);
    return 1;
}
