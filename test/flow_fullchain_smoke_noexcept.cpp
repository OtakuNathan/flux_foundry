#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <system_error>
#include <thread>
#include <utility>

#include "base/traits.h"
#include "flow/flow_def.h"

namespace flux_foundry {
template <>
struct cancel_error<std::error_code> {
    static std::error_code make(cancel_kind kind) noexcept {
        return std::error_code(kind == cancel_kind::hard ? 1001 : 1002, std::generic_category());
    }
};

template <>
struct awaitable_creating_error<std::error_code> {
    static std::error_code make() noexcept {
        return std::error_code(1003, std::generic_category());
    }
};

template <>
struct async_submission_failed_error<std::error_code> {
    static std::error_code make() noexcept {
        return std::error_code(1004, std::generic_category());
    }
};

template <>
struct async_all_failed_error<std::error_code> {
    static std::error_code make() noexcept {
        return std::error_code(1005, std::generic_category());
    }
};

template <>
struct async_any_failed_error<std::error_code> {
    static std::error_code make(size_t i) noexcept {
        return std::error_code(static_cast<int>(1100 + i), std::generic_category());
    }
};
}

#include "executor/simple_executor.h"
#include "flow/flow.h"

using namespace flux_foundry;

namespace {
using err_t = std::error_code;
using out_t = result_t<int, err_t>;

struct run_observer {
    std::atomic<int> done{0};
    std::atomic<int> has_value{-1};
    std::atomic<int> value{0};
    std::atomic<int> err_code{0};
};

struct int_receiver {
    using value_type = out_t;

    run_observer* obs;

    void emplace(value_type&& r) noexcept {
        if (r.has_value()) {
            obs->value.store(r.value(), std::memory_order_relaxed);
            obs->has_value.store(1, std::memory_order_relaxed);
        } else {
            obs->err_code.store(r.error().value(), std::memory_order_relaxed);
            obs->has_value.store(0, std::memory_order_relaxed);
        }
        obs->done.store(1, std::memory_order_release);
    }
};

bool wait_done(run_observer& obs, int timeout_ms) {
    auto begin = std::chrono::steady_clock::now();
    while (obs.done.load(std::memory_order_acquire) == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        auto elapsed = std::chrono::steady_clock::now() - begin;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > timeout_ms) {
            return false;
        }
    }
    return true;
}

void check(bool cond, const char* name, int& failed) {
    if (cond) {
        std::printf("[OK] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++failed;
    }
}

struct executor_env {
    simple_executor<1024> ex;
    std::thread worker;

    executor_env()
        : worker([this]() noexcept { ex.run(); }) {
        std::atomic<int> started{0};
        ex.dispatch(task_wrapper_sbo([&started]() noexcept {
            started.store(1, std::memory_order_release);
        }));

        auto begin = std::chrono::steady_clock::now();
        while (started.load(std::memory_order_acquire) == 0) {
            std::this_thread::yield();
            auto elapsed = std::chrono::steady_clock::now() - begin;
            if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() > 2) {
                std::printf("[FAIL] executor start timeout\n");
                std::abort();
            }
        }
    }

    ~executor_env() {
        bool shutdown_ok = false;
        for (int i = 0; i < 200000; ++i) {
            if (ex.try_shutdown()) {
                shutdown_ok = true;
                break;
            }
            std::this_thread::yield();
        }

        if (!shutdown_ok) {
            std::printf("[FAIL] executor shutdown timeout\n");
            std::abort();
        }

        if (worker.joinable()) {
            worker.join();
        }
    }
};

simple_executor<1024>* g_exec = nullptr;

struct plus_one_async_awaitable : awaitable_base<plus_one_async_awaitable, int, err_t> {
    using async_result_type = out_t;

    int v;

    explicit plus_one_async_awaitable(async_result_type&& in) noexcept
        : v(in.has_value() ? in.value() : 0) {
    }

    int submit() noexcept {
        auto* ex = g_exec;
        if (ex == nullptr) {
            return -1;
        }

        this->retain();
        int value = v;
        ex->dispatch(task_wrapper_sbo([self = this, value]() noexcept {
            self->resume(async_result_type(value_tag, value + 1));
            self->release();
        }));
        return 0;
    }

    void cancel() noexcept {
    }

    bool available() const noexcept {
        return true;
    }
};

struct submit_fail_awaitable : awaitable_base<submit_fail_awaitable, int, err_t> {
    using async_result_type = out_t;

    explicit submit_fail_awaitable(async_result_type&&) noexcept {
    }

    int submit() noexcept {
        return -1;
    }

    void cancel() noexcept {
    }

    bool available() const noexcept {
        return true;
    }
};

int test_full_chain_success() {
    executor_env env;
    g_exec = &env.ex;

    run_observer obs;
    auto bp = make_blueprint<int, err_t>()
        | transform([](int x) noexcept { return x + 1; })
        | then([](out_t&& in) noexcept -> out_t {
            if (!in.has_value()) {
                return out_t(error_tag, in.error());
            }
            return out_t(value_tag, in.value() * 2);
        })
        | on_error([](out_t&& in) noexcept -> out_t {
            if (!in.has_value()) {
                return out_t(value_tag, -100);
            }
            return out_t(value_tag, in.value());
        })
        | via(&env.ex)
        | await<plus_one_async_awaitable>(&env.ex)
        | end();

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, int_receiver{&obs});
    runner(5);

    int failed = 0;
    check(wait_done(obs, 1000), "noexc full_chain wait done", failed);
    check(obs.has_value.load(std::memory_order_acquire) == 1, "noexc full_chain has value", failed);
    check(obs.value.load(std::memory_order_acquire) == 13, "noexc full_chain value == 13", failed);

    g_exec = nullptr;
    return failed;
}

int test_on_error_recover() {
    executor_env env;
    g_exec = &env.ex;

    run_observer obs;
    auto bp = make_blueprint<int, err_t>()
        | via(&env.ex)
        | then([](out_t&&) noexcept -> out_t {
            return out_t(error_tag, std::error_code(42, std::generic_category()));
        })
        | on_error([](out_t&& in) noexcept -> out_t {
            if (!in.has_value()) {
                return out_t(value_tag, 88);
            }
            return out_t(value_tag, in.value());
        })
        | end();

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, int_receiver{&obs});
    runner(1);

    int failed = 0;
    check(wait_done(obs, 1000), "noexc on_error wait done", failed);
    check(obs.has_value.load(std::memory_order_acquire) == 1, "noexc on_error recovered", failed);
    check(obs.value.load(std::memory_order_acquire) == 88, "noexc on_error value == 88", failed);

    g_exec = nullptr;
    return failed;
}

int test_when_all() {
    executor_env env;
    g_exec = &env.ex;

    auto leaf1 = make_blueprint<int, err_t>()
        | via(&env.ex)
        | transform([](int x) noexcept { return x + 10; })
        | await<plus_one_async_awaitable>(&env.ex)
        | end();

    auto leaf2 = make_blueprint<int, err_t>()
        | via(&env.ex)
        | transform([](int x) noexcept { return x + 20; })
        | await<plus_one_async_awaitable>(&env.ex)
        | end();

    auto p1 = make_lite_ptr<decltype(leaf1)>(std::move(leaf1));
    auto p2 = make_lite_ptr<decltype(leaf2)>(std::move(leaf2));

    auto bp = await_when_all(
        &env.ex,
        [](int a, int b) noexcept { return out_t(value_tag, a + b); },
        [](flow_async_agg_err_t e) noexcept { return out_t(error_tag, e); },
        p1,
        p2)
        | end();

    run_observer obs;
    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, int_receiver{&obs});
    runner(make_flat_storage(1, 2));

    int failed = 0;
    check(wait_done(obs, 1000), "noexc when_all wait done", failed);
    check(obs.has_value.load(std::memory_order_acquire) == 1, "noexc when_all has value", failed);
    check(obs.value.load(std::memory_order_acquire) == 35, "noexc when_all value == 35", failed);

    g_exec = nullptr;
    return failed;
}

int test_when_any() {
    executor_env env;
    g_exec = &env.ex;

    auto leaf1 = make_blueprint<int, err_t>()
        | via(&env.ex)
        | transform([](int x) noexcept { return x + 100; })
        | await<plus_one_async_awaitable>(&env.ex)
        | end();

    auto leaf2 = make_blueprint<int, err_t>()
        | via(&env.ex)
        | transform([](int x) noexcept { return x + 200; })
        | await<plus_one_async_awaitable>(&env.ex)
        | end();

    auto p1 = make_lite_ptr<decltype(leaf1)>(std::move(leaf1));
    auto p2 = make_lite_ptr<decltype(leaf2)>(std::move(leaf2));

    auto bp = await_when_any(
        &env.ex,
        [](int x) noexcept { return out_t(value_tag, x); },
        [](flow_async_agg_err_t e) noexcept { return out_t(error_tag, e); },
        p1,
        p2)
        | end();

    run_observer obs;
    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, int_receiver{&obs});
    runner(make_flat_storage(1, 2));

    int failed = 0;
    check(wait_done(obs, 1000), "noexc when_any wait done", failed);
    check(obs.has_value.load(std::memory_order_acquire) == 1, "noexc when_any has value", failed);
    int value = obs.value.load(std::memory_order_acquire);
    check(value == 102 || value == 203, "noexc when_any winner in {102,203}", failed);

    g_exec = nullptr;
    return failed;
}

int test_submit_fail_path() {
    executor_env env;
    g_exec = &env.ex;

    run_observer obs;
    auto bp = make_blueprint<int, err_t>()
        | via(&env.ex)
        | await<submit_fail_awaitable>(&env.ex)
        | end();

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, int_receiver{&obs});
    runner(9);

    int failed = 0;
    check(wait_done(obs, 1000), "noexc submit_fail wait done", failed);
    check(obs.has_value.load(std::memory_order_acquire) == 0, "noexc submit_fail has error", failed);
    check(obs.err_code.load(std::memory_order_acquire) == 1004, "noexc submit_fail code == 1004", failed);

    g_exec = nullptr;
    return failed;
}

} // namespace

int main() {
    int failed = 0;

    failed += test_full_chain_success();
    failed += test_on_error_recover();
    failed += test_when_all();
    failed += test_when_any();
    failed += test_submit_fail_path();

    if (failed == 0) {
        std::printf("[PASS] fullchain no-exception smoke passed\n");
        return 0;
    }

    std::printf("[FAIL] fullchain no-exception smoke failed with %d failures\n", failed);
    return 1;
}
