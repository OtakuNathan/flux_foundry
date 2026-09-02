#include <cstdio>
#include <exception>
#include <utility>

#include "flow/flow.h"

using namespace flux_foundry;

namespace {
using error_t = std::exception_ptr;
using output_t = result_t<int, error_t>;

struct observation {
    int calls = 0;
    int value = 0;
    bool has_value = false;
};

struct receiver {
    using value_type = output_t;

    observation* out;

    void emplace(value_type&& result) noexcept {
        ++out->calls;
        out->has_value = result.has_value();
        if (result.has_value()) {
            out->value = result.value();
        }
    }
};

struct inline_awaitable final : awaitable_base<inline_awaitable, int, error_t> {
    using async_result_type = output_t;
    static constexpr bool completes_inline = true;
    static constexpr bool support_cancel = false;

    int value;

    explicit inline_awaitable(async_result_type&& input) noexcept
        : value(input.has_value() ? input.value() : 0) {
    }

    int submit() noexcept {
        resume(async_result_type(value_tag, value + 1));
        return 0;
    }

    void cancel() noexcept {
    }
};

struct inline_fast_awaitable final : fast_awaitable_base<inline_fast_awaitable, int, error_t> {
    using async_result_type = output_t;
    static constexpr bool completes_inline = true;

    int value;

    explicit inline_fast_awaitable(async_result_type&& input) noexcept
        : value(input.has_value() ? input.value() : 0) {
    }

    int submit() noexcept {
        resume(async_result_type(value_tag, value + 1));
        return 0;
    }
};

struct cancellable_inline_awaitable final
    : awaitable_base<cancellable_inline_awaitable, int, error_t> {
    using async_result_type = output_t;
    static constexpr bool completes_inline = true;

    int value;

    explicit cancellable_inline_awaitable(async_result_type&& input) noexcept
        : value(input.has_value() ? input.value() : 0) {
    }

    int submit() noexcept {
        resume(async_result_type(value_tag, value + 1));
        return 0;
    }

    void cancel() noexcept {
    }
};

static_assert(awaitable_completes_inline_v<cancellable_inline_awaitable>,
    "test awaitable must advertise inline completion");

struct held_awaitable final : awaitable_base<held_awaitable, int, error_t> {
    using async_result_type = output_t;

    static held_awaitable* pending;
    int value;

    explicit held_awaitable(async_result_type&& input) noexcept
        : value(input.has_value() ? input.value() : 0) {
    }

    int submit() noexcept {
        retain();
        pending = this;
        return 0;
    }

    void cancel() noexcept {
    }

    static void complete() noexcept {
        auto* self = pending;
        pending = nullptr;
        const int result = self->value + 1;
        self->resume(async_result_type(value_tag, result));
        self->release();
    }
};

held_awaitable* held_awaitable::pending = nullptr;

struct held_fast_awaitable final : fast_awaitable_base<held_fast_awaitable, int, error_t> {
    using async_result_type = output_t;

    static held_fast_awaitable* pending;
    int value;

    explicit held_fast_awaitable(async_result_type&& input) noexcept
        : value(input.has_value() ? input.value() : 0) {
    }

    int submit() noexcept {
        pending = this;
        return 0;
    }

    static void complete() noexcept {
        auto* self = pending;
        pending = nullptr;
        const int result = self->value + 1;
        self->resume(async_result_type(value_tag, result));
    }
};

held_fast_awaitable* held_fast_awaitable::pending = nullptr;

struct failed_awaitable final : awaitable_base<failed_awaitable, int, error_t> {
    using async_result_type = output_t;

    explicit failed_awaitable(async_result_type&&) noexcept {
    }

    int submit() noexcept {
        return -1;
    }

    void cancel() noexcept {
    }
};

struct failed_fast_awaitable final : fast_awaitable_base<failed_fast_awaitable, int, error_t> {
    using async_result_type = output_t;

    explicit failed_fast_awaitable(async_result_type&&) noexcept {
    }

    int submit() noexcept {
        return -1;
    }
};

struct queued_executor {
    task_wrapper_sbo task;
    int dispatches = 0;

    void dispatch(task_wrapper_sbo next) noexcept {
        ++dispatches;
        task = std::move(next);
    }

    void run_one() noexcept {
        task_wrapper_sbo next = std::move(task);
        next();
    }
};

void check(bool condition, const char* name, int& failures) {
    if (condition) {
        std::printf("[OK] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++failures;
    }
}

int test_synchronous_reuse() {
    observation normal_out;
    auto normal_bp = make_blueprint<int>()
        | transform([](int value) noexcept { return value + 1; })
        | end();
    auto normal_ptr = make_lite_ptr<decltype(normal_bp)>(std::move(normal_bp));
    auto normal = make_runner(normal_ptr, receiver{ &normal_out });
    normal(1);
    normal(2);

    observation fast_out;
    auto fast_bp = make_blueprint<int>()
        | transform([](int value) noexcept { return value + 1; })
        | end();
    auto fast_ptr = make_lite_ptr<decltype(fast_bp)>(std::move(fast_bp));
    auto fast = make_fast_runner(fast_ptr, receiver{ &fast_out });
    fast(1);
    fast(2);

    int failures = 0;
    check(!normal.is_consumed() && normal_out.calls == 2 && normal_out.value == 3,
        "normal synchronous runner remains reusable", failures);
    check(!fast.is_consumed() && fast_out.calls == 2 && fast_out.value == 3,
        "fast synchronous runner remains reusable", failures);
    return failures;
}

int test_declared_inline_awaitable_reuse() {
    observation normal_out;
    auto normal_bp = make_blueprint<int>() | await<inline_awaitable>() | end();
    auto normal_ptr = make_lite_ptr<decltype(normal_bp)>(std::move(normal_bp));
    auto normal = make_runner(normal_ptr, receiver{ &normal_out });
    normal(3);
    normal(4);

    observation fast_out;
    auto fast_bp = make_blueprint<int>() | await<inline_fast_awaitable>() | end();
    auto fast_ptr = make_lite_ptr<decltype(fast_bp)>(std::move(fast_bp));
    auto fast = make_fast_runner(fast_ptr, receiver{ &fast_out });
    fast(3);
    fast(4);

    int failures = 0;
    check(!normal.is_consumed() && normal_out.calls == 2 && normal_out.value == 5,
        "declared-inline normal awaitable remains reusable", failures);
    check(!fast.is_consumed() && fast_out.calls == 2 && fast_out.value == 5,
        "declared-inline fast awaitable remains reusable", failures);
    return failures;
}

int test_async_awaitable_consumes_runner() {
    observation normal_out;
    auto normal_bp = make_blueprint<int>() | await<held_awaitable>() | end();
    auto normal_ptr = make_lite_ptr<decltype(normal_bp)>(std::move(normal_bp));
    auto normal = make_runner(normal_ptr, receiver{ &normal_out });
    normal(10);
    auto* original_normal_pending = held_awaitable::pending;
#ifdef NDEBUG
    normal(99);
#endif
    const bool normal_pending_not_replaced = held_awaitable::pending == original_normal_pending;
    held_awaitable::complete();

    observation fast_out;
    auto fast_bp = make_blueprint<int>() | await<held_fast_awaitable>() | end();
    auto fast_ptr = make_lite_ptr<decltype(fast_bp)>(std::move(fast_bp));
    auto fast = make_fast_runner(fast_ptr, receiver{ &fast_out });
    fast(20);
    auto* original_fast_pending = held_fast_awaitable::pending;
#ifdef NDEBUG
    fast(99);
#endif
    const bool fast_pending_not_replaced = held_fast_awaitable::pending == original_fast_pending;
    held_fast_awaitable::complete();

    int failures = 0;
    check(normal.is_consumed() && original_normal_pending != nullptr && normal_pending_not_replaced
            && normal_out.calls == 1 && normal_out.value == 11,
        "normal async handoff consumes runner", failures);
    check(fast.is_consumed() && original_fast_pending != nullptr && fast_pending_not_replaced
            && fast_out.calls == 1 && fast_out.value == 21,
        "fast async handoff consumes runner", failures);
#ifdef NDEBUG
    check(normal_pending_not_replaced,
        "release build rejects normal consumed-runner invocation", failures);
    check(fast_pending_not_replaced,
        "release build rejects fast consumed-runner invocation", failures);
#endif
    return failures;
}

int test_cancellable_inline_awaitable_is_conservative() {
    observation out;
    auto bp = make_blueprint<int>() | await<cancellable_inline_awaitable>() | end();
    auto ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(ptr, receiver{ &out });
    runner(7);

    int failures = 0;
    check(runner.is_consumed() && out.calls == 1 && out.value == 8,
        "cancellable inline awaitable remains one-shot", failures);
    return failures;
}

int test_failed_submission_does_not_consume_runner() {
    observation normal_out;
    auto normal_bp = make_blueprint<int>() | await<failed_awaitable>() | end();
    auto normal_ptr = make_lite_ptr<decltype(normal_bp)>(std::move(normal_bp));
    auto normal = make_runner(normal_ptr, receiver{ &normal_out });
    normal(1);
    normal(2);

    observation fast_out;
    auto fast_bp = make_blueprint<int>() | await<failed_fast_awaitable>() | end();
    auto fast_ptr = make_lite_ptr<decltype(fast_bp)>(std::move(fast_bp));
    auto fast = make_fast_runner(fast_ptr, receiver{ &fast_out });
    fast(1);
    fast(2);

    int failures = 0;
    check(!normal.is_consumed() && normal_out.calls == 2 && !normal_out.has_value,
        "normal failed submission remains reusable", failures);
    check(!fast.is_consumed() && fast_out.calls == 2 && !fast_out.has_value,
        "fast failed submission remains reusable", failures);
    return failures;
}

int test_inline_prefix_propagates_later_handoff() {
    observation normal_out;
    auto normal_bp = make_blueprint<int>()
        | await<inline_awaitable>()
        | await<held_awaitable>()
        | end();
    auto normal_ptr = make_lite_ptr<decltype(normal_bp)>(std::move(normal_bp));
    auto normal = make_runner(normal_ptr, receiver{ &normal_out });
    normal(40);
    const bool normal_consumed_before_completion = normal.is_consumed();
    held_awaitable::complete();

    observation fast_out;
    auto fast_bp = make_blueprint<int>()
        | await<inline_fast_awaitable>()
        | await<held_fast_awaitable>()
        | end();
    auto fast_ptr = make_lite_ptr<decltype(fast_bp)>(std::move(fast_bp));
    auto fast = make_fast_runner(fast_ptr, receiver{ &fast_out });
    fast(50);
    const bool fast_consumed_before_completion = fast.is_consumed();
    held_fast_awaitable::complete();

    int failures = 0;
    check(normal_consumed_before_completion && normal_out.calls == 1 && normal_out.value == 42,
        "normal inline prefix propagates later async consumption", failures);
    check(fast_consumed_before_completion && fast_out.calls == 1 && fast_out.value == 52,
        "fast inline prefix propagates later async consumption", failures);
    return failures;
}

int test_via_consumes_runner() {
    queued_executor executor;
    observation out;
    auto bp = make_blueprint<int>()
        | via(&executor)
        | transform([](int value) noexcept { return value + 1; })
        | end();
    auto ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(ptr, receiver{ &out });
    runner(30);
#ifdef NDEBUG
    runner(99);
#endif

    int failures = 0;
    check(runner.is_consumed() && executor.dispatches == 1 && out.calls == 0,
        "non-inline via consumes runner before dispatch", failures);
    check(static_cast<bool>(runner.get_controller()),
        "consumed normal runner retains cancellation controller", failures);
    executor.run_one();
    check(out.calls == 1 && out.value == 31,
        "non-inline via continuation still completes", failures);
    return failures;
}
} // namespace

int main() {
    int failures = 0;
    failures += test_synchronous_reuse();
    failures += test_declared_inline_awaitable_reuse();
    failures += test_async_awaitable_consumes_runner();
    failures += test_cancellable_inline_awaitable_is_conservative();
    failures += test_failed_submission_does_not_consume_runner();
    failures += test_inline_prefix_propagates_later_handoff();
    failures += test_via_consumes_runner();

    if (failures == 0) {
        std::printf("[PASS] runner reuse contract\n");
        return 0;
    }
    std::printf("[FAIL] runner reuse contract: %d failures\n", failures);
    return 1;
}
