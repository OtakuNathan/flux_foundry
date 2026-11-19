// Library-synthesized errors must stay catchable both ways:
//   - legacy: catch (const std::logic_error&) + what() text
//   - new:    catch (const flux_foundry::flow_error&) + code/index fields
#include "flow/flow.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>

namespace flow_error_context_test {

int failed = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failed;
    }
}

#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS

bool grab(const std::exception_ptr& ep,
          flux_foundry::flow_error_code& code,
          size_t& index,
          std::string& msg) {
    if (!ep) {
        return false;
    }
    try {
        std::rethrow_exception(ep);
    } catch (const flux_foundry::flow_error& e) {
        code = e.code();
        index = e.index();
        msg = e.what();
        return true;
    } catch (...) {
        return false;
    }
}

bool still_logic_error(const std::exception_ptr& ep, const char* expected) {
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

void run() {
    using namespace flux_foundry;

    flow_error_code code = flow_error_code::all_async_failed;
    size_t index = 12345;
    std::string msg;

    // Soft cancel carries its classification, and the legacy message survives.
    const std::exception_ptr soft = cancel_error<std::exception_ptr>::make(cancel_kind::soft);
    check(grab(soft, code, index, msg), "soft cancel is a flow_error");
    check(code == flow_error_code::cancel_soft, "soft cancel code");
    check(index == 0, "soft cancel index");
    check(msg == "flow soft-canceled", "soft cancel what()");
    check(still_logic_error(soft, "flow soft-canceled"), "soft cancel legacy catch");

    // Hard cancel.
    const std::exception_ptr hard = cancel_error<std::exception_ptr>::make(cancel_kind::hard);
    check(grab(hard, code, index, msg), "hard cancel is a flow_error");
    check(code == flow_error_code::cancel_hard, "hard cancel code");
    check(msg == "flow hard-canceled", "hard cancel what()");

    // Submission failure.
    const std::exception_ptr submit =
        async_submission_failed_error<std::exception_ptr>::make(-71);
    check(grab(submit, code, index, msg), "submit failure is a flow_error");
    check(code == flow_error_code::async_submission_failed, "submit code");
    check(still_logic_error(submit, "failed to submit async operation"),
        "submit legacy catch");
    try {
        std::rethrow_exception(submit);
    } catch (const flow_error& e) {
        check(e.submit_code() == -71, "submit factory preserves signed status");
    }

    // Awaitable creation failure.
    const std::exception_ptr create = awaitable_creating_error<std::exception_ptr>::make();
    check(grab(create, code, index, msg), "awaitable create is a flow_error");
    check(code == flow_error_code::awaitable_create_failed, "awaitable create code");

    // All-failed.
    const std::exception_ptr all = async_all_failed_error<std::exception_ptr>::make();
    check(grab(all, code, index, msg), "all-failed is a flow_error");
    check(code == flow_error_code::all_async_failed, "all-failed code");

    // Any-failed must report which aggregate member failed.
    const std::exception_ptr any = async_any_failed_error<std::exception_ptr>::make(3);
    check(grab(any, code, index, msg), "any-failed is a flow_error");
    check(code == flow_error_code::any_async_failed, "any-failed code");
    check(index == 3, "any-failed index");
    check(msg == "async operation #3 failed", "any-failed legacy what()");
}

#endif

namespace ff = flux_foundry;

struct target {
    int calls = 0;
    int invoke(int argument) noexcept {
        ++calls;
        return argument + 1;
    }
};

struct request_payload {
    target* object;
    int argument;
    int rejection;
    int recovered = 0;
    int destroyed = 0;
    int built = 0;
    std::uintptr_t active_task_buffer = 0;
};

using request = ff::request_context<request_payload>;
using request_error = ff::contextual_error<request>;
static_assert(sizeof(request_error) == sizeof(request*), "error carries one pointer");
static_assert(std::is_trivially_copyable<request_error>::value, "error is a lightweight handle");
static_assert(!std::is_constructible<request_error, request*>::value, "only fail() issues a request error");

void test_context_layout() {
    struct payload { std::uint64_t first; std::uint64_t second; };
    struct expected_layout { payload data; int status; };
    using context = ff::request_context<payload>;
    static_assert(std::is_standard_layout<context>::value, "standard-layout payload keeps the context standard-layout");
    static_assert(sizeof(context) == sizeof(expected_layout), "context adds only the trailing slot and alignment padding");

    context ctx(payload{10, 20});
    const auto& read_only = ctx;
    check(static_cast<const void*>(&read_only) == static_cast<const void*>(&read_only.payload()),
        "payload begins at the context address");
    const auto error = ctx.fail(-42);
    check(error && error.context() == &ctx && error.submit_code() == -42 && read_only.payload().second == 20,
        "failure gives access to the status without changing the payload");
    const request_error empty{};
    check(!empty && empty.context() == nullptr, "empty error carries no request");
}

struct queued_executor {
    ff::task_wrapper_sbo pending;
    void dispatch(ff::task_wrapper_sbo task) noexcept {
        check(!pending, "one delivery per request");
        pending = std::move(task);
    }

    void drain(request& ctx) noexcept {
        if (!pending) return;
        auto task = std::move(pending);
        ctx.payload().active_task_buffer = reinterpret_cast<std::uintptr_t>(task.data_);
        task();
    }
};

#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
class submit_failure : public ff::flow_error {
public:
    request_error detail;
    explicit submit_failure(request_error failure)
        : flow_error(ff::flow_error_code::async_submission_failed, 0, failure.submit_code()), detail(failure) {}
};

using error_type = std::exception_ptr;
#else
using error_type = request_error;
#endif

using result = ff::result_t<request*, error_type>;

template <bool Fast, bool Cancellable> struct operation;
template <bool Fast, bool Cancellable>
using operation_base = std::conditional_t<Fast,
    ff::fast_awaitable_base<operation<Fast, Cancellable>, request*, error_type>,
    ff::awaitable_base<operation<Fast, Cancellable>, request*, error_type>>;

template <bool Fast, bool Cancellable>
struct operation final : operation_base<Fast, Cancellable> {
    using async_result_type = result;
    static constexpr bool support_cancel = Cancellable;
    request* context;

    explicit operation(result&& input) noexcept : context(input.value()) {}
    ~operation() noexcept { ++context->payload().destroyed; }
    bool available() const noexcept { return true; }
    void cancel() noexcept {}

    int submit() noexcept {
        const int rejected = context->payload().rejection;
        if (rejected != 0) return rejected;
        // Exercise completion through the closure that captures the cancel token.
        this->resume(result(ff::error_tag, make_submit_error(-93)));
        return 0;
    }

    error_type make_submit_error(int code) noexcept {
        check(context->payload().destroyed == 0, "awaitable is alive during error construction");
        ++context->payload().built;
        const auto failure = context->fail(code);
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        try {
            return std::make_exception_ptr(submit_failure(failure));
        } catch (...) {
            return std::current_exception();
        }
#else
        return failure;
#endif
    }
};

struct receiver {
    using value_type = result;
    request** delivered;
    void emplace(result&& out) noexcept {
        check(out.has_value(), "failure recovered");
        if (out.has_value()) *delivered = out.value();
    }
};

request* retry(request* ctx) noexcept {
    auto& payload = ctx->payload();
    check(payload.destroyed == 1, "awaitable released before recovery");
    payload.recovered = payload.object->invoke(payload.argument);
    return ctx;
}

#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
struct inspect_failure {
    result operator()(result&& failed) const noexcept {
        try {
            std::rethrow_exception(failed.error());
        } catch (const submit_failure& error) {
            check(error.submit_code() == error.detail.submit_code(), "on_error sees the contextual exception");
        } catch (...) {
            check(false, "on_error sees the original exception type");
        }
        return std::move(failed);
    }
};

struct recover {
    request* operator()(const submit_failure& error) const noexcept {
        check(error.submit_code() == error.detail.submit_code(), "typed exception carries the original code");
        return retry(error.detail.context());
    }
};
#else
struct recover {
    result operator()(result&& failed) const noexcept {
        const auto& error = failed.error();
        if (!error) {
            check(false, "recovery receives the request context");
            return std::move(failed);
        }
        auto* ctx = error.context();
        const auto expected = ctx->payload().rejection ? ctx->payload().rejection : -93;
        check(error.submit_code() == expected, "on_error reads the original status through the handle");
        const auto address = reinterpret_cast<std::uintptr_t>(&error);
        const auto buffer = ctx->payload().active_task_buffer;
        check(address >= buffer && address < buffer + ff::task_wrapper_sbo::sbo_size,
            "context error lives in the running queued task's inline buffer");
        return result(ff::value_tag, retry(ctx));
    }
};
#endif

template <typename BP>
void submit(const BP& bp, request& ctx, receiver out, std::false_type) {
    auto runner = ff::make_runner_view(bp, out);
    runner(&ctx);
}

template <typename BP>
void submit(const BP& bp, request& ctx, receiver out, std::true_type) {
    auto runner = ff::make_fast_runner_view(bp, out);
    runner(&ctx);
}

template <bool FastRunner, bool FastAwaitable, bool Cancellable>
void test_recovery(int rejection) {
    static queued_executor executor;
#if FLUX_FOUNDRY_CPP_VER >= 201703L
    static constexpr
#else
    static const
#endif
    auto bp = ff::make_blueprint<request*, error_type>()
        | ff::await<operation<FastAwaitable, Cancellable>>(&executor)
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        | ff::on_error(inspect_failure{})
        | ff::catch_exception<submit_failure>(recover{})
#else
        | ff::on_error(recover{})
#endif
        | ff::end();

    target object;
    request ctx(request_payload{&object, 41, rejection});
    request* delivered = nullptr;
    submit(bp, ctx, receiver{&delivered}, std::integral_constant<bool, FastRunner>{});
    check(executor.pending && !delivered, "result queued before recovery");
    if (rejection != 0) check(ctx.payload().destroyed == 1, "rejected awaitable released before queue drain");
    check(ctx.payload().built == 1, "error constructed once");
    executor.drain(ctx);
    check(delivered == &ctx && object.calls == 1 && ctx.payload().recovered == 42,
        "retry used the original object and arguments");
}

template <typename E>
struct factory_operation final : ff::fast_awaitable_base<factory_operation<E>, int, E> {
    using async_result_type = ff::result_t<int, E>;
    int code;
    explicit factory_operation(async_result_type&& in) noexcept : code(in.value()) {}
    bool available() const noexcept { return true; }
    int submit() noexcept { return code; }
};

template <typename E>
struct factory_receiver {
    using value_type = ff::result_t<int, E>;
    E* received;
    void emplace(value_type&& out) noexcept {
        check(out.has_error(), "factory produces a failure");
        if (out.has_error()) *received = std::move(out).error();
    }
};

template <typename E>
E run_factory(int code) {
    auto bp = ff::make_blueprint<int, E>() | ff::await<factory_operation<E>>() | ff::end();
    E error{};
    auto runner = ff::make_fast_runner_view(bp, factory_receiver<E>{&error});
    runner(code);
    return error;
}

#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
struct cv_recover {
    int operator()(const ff::flow_error&) noexcept { return 1; }
    int operator()(const ff::flow_error&) const noexcept { return 2; }
};

void test_catch_dispatch() {
    using value = ff::result_t<int, std::exception_ptr>;
    struct sink {
        using value_type = value;
        int* observed;
        void emplace(value&& out) noexcept { if (out.has_value()) *observed = out.value(); }
    };

    auto bp = ff::make_blueprint<int>() | ff::await<factory_operation<std::exception_ptr>>()
        | ff::catch_exception<ff::flow_error>(cv_recover{}) | ff::end();
    int observed = 0;
    ff::make_fast_runner_view(bp, sink{&observed})(-23);
    check(observed == 1, "mutable catch uses the non-const overload");
    const auto& const_bp = bp;
    ff::make_fast_runner_view(const_bp, sink{&observed})(-23);
    check(observed == 2, "const catch uses the const overload");
}
#else
struct legacy_error { int code; };
struct status_error { int code; };
#endif

} // namespace flow_error_context_test

#if !FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
namespace flux_foundry {
template <>
struct cancel_error<flow_error_context_test::error_type> {
    static flow_error_context_test::error_type make(cancel_kind) noexcept {
        return {};
    }
};

template <>
struct awaitable_creating_error<flow_error_context_test::error_type> {
    static flow_error_context_test::error_type make() noexcept {
        return {};
    }
};
template <>
struct awaitable_creating_error<flow_error_context_test::legacy_error> {
    static flow_error_context_test::legacy_error make() noexcept { return {-1}; }
};

template <>
struct awaitable_creating_error<flow_error_context_test::status_error> {
    static flow_error_context_test::status_error make() noexcept { return {-1}; }
};

template <>
struct async_submission_failed_error<flow_error_context_test::legacy_error> {
    static flow_error_context_test::legacy_error make() noexcept { return {5}; }
};

template <>
struct async_submission_failed_error<flow_error_context_test::status_error> {
    static flow_error_context_test::status_error make(int code) noexcept { return {code}; }
    static flow_error_context_test::status_error make() noexcept { return {5}; }
};
} // namespace flux_foundry
#endif

int main() {
    using namespace flow_error_context_test;
    test_context_layout();
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    run();
    test_catch_dispatch();
    for (int code : {-23, 71}) {
        const auto error = run_factory<std::exception_ptr>(code);
        check(static_cast<bool>(error), "default exception factory produced an exception");
        if (!error) continue;
        try {
            std::rethrow_exception(error);
        } catch (const ff::flow_error& failure) {
            check(failure.submit_code() == code, "default exception reports actual runner submit status");
        } catch (...) {
            check(false, "default factory produced the expected exception type");
        }
    }
#else
    check(run_factory<legacy_error>(-23).code == 5, "legacy no-arg error factory still works");
    check(run_factory<status_error>(-71).code == -71, "status-aware factory takes priority");
#endif
    test_recovery<false, false, true>(-23);
    test_recovery<false, false, false>(-71);
    test_recovery<true, true, false>(-23);
    test_recovery<false, false, true>(0);
    if (failed != 0) {
        std::printf("flow_error_context: %d FAILED\n", failed);
        return 1;
    }
    std::printf("flow_error_context: PASS\n");
}
