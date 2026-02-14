#include <cstdio>
#include <system_error>
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

#include "flow/flow.h"

using namespace flux_foundry;

namespace {
using err_t = std::error_code;
using out_t = result_t<int, err_t>;

struct inline_executor {
    void dispatch(task_wrapper_sbo t) noexcept {
        t();
    }
};

struct plus_one_awaitable final : awaitable_base<plus_one_awaitable, int, err_t> {
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

    bool available() const noexcept {
        return true;
    }
};

struct submit_fail_awaitable final : awaitable_base<submit_fail_awaitable, int, err_t> {
    using async_result_type = out_t;

    explicit submit_fail_awaitable(async_result_type&&) noexcept {
    }

    int submit() noexcept {
        return -1;
    }

    void cancel() noexcept {}

    bool available() const noexcept {
        return true;
    }
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

    auto bp = make_blueprint<int, err_t>()
        | await<plus_one_awaitable>(&ex)
        | await<plus_one_awaitable>(&ex)
        | end();

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, int_receiver{&obs});
    runner(5);

    int failed = 0;
    check(obs.called, "noexc async|async called", failed);
    check(obs.has_value, "noexc async|async has value", failed);
    check(obs.value == 7, "noexc async|async value == 7", failed);
    return failed;
}

int test_when_all() {
    inline_executor ex;
    run_observer obs;

    auto leaf1 = make_blueprint<int, err_t>()
        | transform([](int x) noexcept { return x + 10; })
        | end();

    auto leaf2 = make_blueprint<int, err_t>()
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
            return out_t(error_tag, e);
        },
        p1,
        p2)
        | end();

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, int_receiver{&obs});
    runner(make_flat_storage(1, 2));

    int failed = 0;
    check(obs.called, "noexc when_all called", failed);
    check(obs.has_value, "noexc when_all has value", failed);
    check(obs.value == 33, "noexc when_all value == 33", failed);
    return failed;
}

int test_when_any() {
    inline_executor ex;
    run_observer obs;

    auto leaf1 = make_blueprint<int, err_t>()
        | transform([](int x) noexcept { return x + 100; })
        | end();

    auto leaf2 = make_blueprint<int, err_t>()
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
            return out_t(error_tag, e);
        },
        p1,
        p2)
        | end();

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, int_receiver{&obs});
    runner(make_flat_storage(1, 2));

    int failed = 0;
    check(obs.called, "noexc when_any called", failed);
    check(obs.has_value, "noexc when_any has value", failed);
    check(obs.value == 101, "noexc when_any value == 101", failed);
    return failed;
}

int test_when_all_fast() {
    inline_executor ex;
    run_observer obs;

    auto leaf1 = make_blueprint<int, err_t>()
        | transform([](int x) noexcept { return x + 10; })
        | end();

    auto leaf2 = make_blueprint<int, err_t>()
        | transform([](int x) noexcept { return x + 20; })
        | end();

    auto p1 = make_lite_ptr<decltype(leaf1)>(std::move(leaf1));
    auto p2 = make_lite_ptr<decltype(leaf2)>(std::move(leaf2));

    auto bp = await_when_all_fast(
        &ex,
        [](int a, int b) noexcept {
            return out_t(value_tag, a + b);
        },
        [](flow_async_agg_err_t e) noexcept {
            return out_t(error_tag, e);
        },
        p1,
        p2)
        | end();

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, int_receiver{&obs});
    runner(make_flat_storage(1, 2));

    int failed = 0;
    check(obs.called, "noexc when_all_fast called", failed);
    check(obs.has_value, "noexc when_all_fast has value", failed);
    check(obs.value == 33, "noexc when_all_fast value == 33", failed);
    return failed;
}

int test_when_any_fast() {
    inline_executor ex;
    run_observer obs;

    auto leaf1 = make_blueprint<int, err_t>()
        | transform([](int x) noexcept { return x + 100; })
        | end();

    auto leaf2 = make_blueprint<int, err_t>()
        | transform([](int x) noexcept { return x + 200; })
        | end();

    auto p1 = make_lite_ptr<decltype(leaf1)>(std::move(leaf1));
    auto p2 = make_lite_ptr<decltype(leaf2)>(std::move(leaf2));

    auto bp = await_when_any_fast(
        &ex,
        [](int x) noexcept {
            return out_t(value_tag, x);
        },
        [](flow_async_agg_err_t e) noexcept {
            return out_t(error_tag, e);
        },
        p1,
        p2)
        | end();

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, int_receiver{&obs});
    runner(make_flat_storage(1, 2));

    int failed = 0;
    check(obs.called, "noexc when_any_fast called", failed);
    check(obs.has_value, "noexc when_any_fast has value", failed);
    check(obs.value == 101, "noexc when_any_fast value == 101", failed);
    return failed;
}

int test_when_all_rejects_null_bp() {
    inline_executor ex;
    run_observer obs;

    auto leaf = make_blueprint<int, err_t>()
        | transform([](int x) noexcept { return x + 10; })
        | end();

    using leaf_t = decltype(leaf);
    auto p_valid = make_lite_ptr<leaf_t>(std::move(leaf));
    lite_ptr<leaf_t> p_null;

    auto bp = await_when_all(
        &ex,
        [](int a, int b) noexcept {
            return out_t(value_tag, a + b);
        },
        [](flow_async_agg_err_t e) noexcept {
            return out_t(error_tag, e);
        },
        p_valid,
        p_null)
        | end();

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, int_receiver{&obs});
    runner(make_flat_storage(1, 2));

    int failed = 0;
    check(obs.called, "noexc when_all null bp called", failed);
    check(!obs.has_value, "noexc when_all null bp has error", failed);
    check(obs.err.value() == 1004, "noexc when_all null bp code == 1004", failed);
    return failed;
}

int test_when_any_accepts_null_bp() {
    inline_executor ex;
    run_observer obs;

    auto leaf = make_blueprint<int, err_t>()
        | transform([](int x) noexcept { return x + 100; })
        | end();

    using leaf_t = decltype(leaf);
    lite_ptr<leaf_t> p_null;
    auto p_valid = make_lite_ptr<leaf_t>(std::move(leaf));

    auto bp = await_when_any(
        &ex,
        [](int x) noexcept {
            return out_t(value_tag, x);
        },
        [](flow_async_agg_err_t e) noexcept {
            return out_t(error_tag, e);
        },
        p_null,
        p_valid)
        | end();

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, int_receiver{&obs});
    runner(make_flat_storage(7, 1));

    int failed = 0;
    check(obs.called, "noexc when_any null bp called", failed);
    check(obs.has_value, "noexc when_any null bp has value", failed);
    check(obs.value == 101, "noexc when_any null bp value == 101", failed);
    return failed;
}

int test_submit_fail_path() {
    inline_executor ex;
    run_observer obs;

    auto bp = make_blueprint<int, err_t>()
        | await<submit_fail_awaitable>(&ex)
        | end();

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, int_receiver{&obs});
    runner(9);

    int failed = 0;
    check(obs.called, "noexc submit fail path called", failed);
    check(!obs.has_value, "noexc submit fail path has error", failed);
    check(obs.err.value() == 1004, "noexc submit fail code == 1004", failed);
    return failed;
}

} // namespace

int main() {
    int failed = 0;

    failed += test_async_async();
    failed += test_when_all();
    failed += test_when_any();
    failed += test_when_all_fast();
    failed += test_when_any_fast();
    failed += test_when_all_rejects_null_bp();
    failed += test_when_any_accepts_null_bp();
    failed += test_submit_fail_path();

    if (failed == 0) {
        std::printf("[PASS] no-exception smoke test passed\n");
        return 0;
    }

    std::printf("[FAIL] no-exception smoke test found %d issues\n", failed);
    return 1;
}
