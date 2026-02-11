#include <cstdio>
#include <exception>
#include <stdexcept>
#include <utility>

#include "flow/flow.h"

using namespace flux_foundry;

namespace {
using err_t = std::exception_ptr;
using out_t = result_t<int, err_t>;

struct inline_executor {
    void dispatch(task_wrapper_sbo t) noexcept {
        t();
    }
};

struct plus_one_awaitable : awaitable_base<plus_one_awaitable, int, err_t> {
    using async_result_type = out_t;
    int v;

    explicit plus_one_awaitable(async_result_type&& in) noexcept
        : v(in.has_value() ? in.value() : 0) {
    }

    int submit() noexcept {
        this->resume(async_result_type(value_tag, v + 1));
        return 0;
    }

    void cancel() noexcept {}
};

struct submit_fail_awaitable : awaitable_base<submit_fail_awaitable, int, err_t> {
    using async_result_type = out_t;

    explicit submit_fail_awaitable(async_result_type&&) noexcept {}

    int submit() noexcept {
        return -1;
    }

    void cancel() noexcept {}
};

struct run_observer {
    bool called = false;
    bool has_value = false;
    int value = 0;
    err_t err;
};

struct int_receiver {
    using value_type = out_t;

    run_observer* obs;

    void emplace(value_type&& r) noexcept {
        obs->called = true;
        obs->has_value = r.has_value();
        if (r.has_value()) {
            obs->value = r.value();
        } else {
            obs->err = r.error();
        }
    }
};

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

void check(bool cond, const char* name, int& failed) {
    if (cond) {
        std::printf("[OK] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++failed;
    }
}

int test_async_async() {
    inline_executor ex;
    run_observer obs;

    auto bp = make_blueprint<int>()
        | await<plus_one_awaitable>(&ex)
        | await<plus_one_awaitable>(&ex)
        | end();

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, int_receiver{&obs});
    runner(5);

    int failed = 0;
    check(obs.called, "async|async called", failed);
    check(obs.has_value, "async|async has value", failed);
    check(obs.value == 7, "async|async value == 7", failed);
    return failed;
}

int test_when_all() {
    inline_executor ex;
    run_observer obs;

    auto leaf1 = make_blueprint<int>()
        | transform([](int x) noexcept { return x + 10; })
        | end();

    auto leaf2 = make_blueprint<int>()
        | transform([](int x) noexcept { return x + 20; })
        | end();

    auto p1 = make_lite_ptr<decltype(leaf1)>(std::move(leaf1));
    auto p2 = make_lite_ptr<decltype(leaf2)>(std::move(leaf2));

    auto bp = await_when_all(
        &ex,
        [](int a, int b) noexcept {
            return out_t(value_tag, a + b);
        },
        [](flow_async_agg_err_t e) noexcept {
            return out_t(error_tag, std::move(e));
        },
        p1,
        p2)
        | end();

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, int_receiver{&obs});
    runner(make_flat_storage(1, 2));

    int failed = 0;
    check(obs.called, "when_all called", failed);
    check(obs.has_value, "when_all has value", failed);
    check(obs.value == 33, "when_all value == 33", failed);
    return failed;
}

int test_when_any() {
    inline_executor ex;
    run_observer obs;

    auto leaf1 = make_blueprint<int>()
        | transform([](int x) noexcept { return x + 100; })
        | end();

    auto leaf2 = make_blueprint<int>()
        | transform([](int x) noexcept { return x + 200; })
        | end();

    auto p1 = make_lite_ptr<decltype(leaf1)>(std::move(leaf1));
    auto p2 = make_lite_ptr<decltype(leaf2)>(std::move(leaf2));

    auto bp = await_when_any(
        &ex,
        [](int x) noexcept {
            return out_t(value_tag, x);
        },
        [](flow_async_agg_err_t e) noexcept {
            return out_t(error_tag, std::move(e));
        },
        p1,
        p2)
        | end();

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, int_receiver{&obs});
    runner(make_flat_storage(1, 2));

    int failed = 0;
    check(obs.called, "when_any called", failed);
    check(obs.has_value, "when_any has value", failed);
    check(obs.value == 101, "when_any value == 101", failed);
    return failed;
}

int test_submit_fail_path() {
    inline_executor ex;
    run_observer obs;

    auto bp = make_blueprint<int>()
        | await<submit_fail_awaitable>(&ex)
        | end();

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, int_receiver{&obs});
    runner(9);

    int failed = 0;
    check(obs.called, "submit fail path called", failed);
    check(!obs.has_value, "submit fail path has error", failed);
    check(has_logic_error_message(obs.err, "failed to submit async operation"),
          "submit fail path error message", failed);
    return failed;
}

} // namespace

int main() {
    int failed = 0;

    failed += test_async_async();
    failed += test_when_all();
    failed += test_when_any();
    failed += test_submit_fail_path();

    if (failed == 0) {
        std::printf("[PASS] smoke test passed\n");
        return 0;
    }

    std::printf("[FAIL] smoke test failed with %d failures\n", failed);
    return 1;
}
