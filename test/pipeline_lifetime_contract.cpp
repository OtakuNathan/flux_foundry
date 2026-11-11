#include "lifecycle_test_support.h"
#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

using namespace flux_foundry;
namespace {
using error = lifecycle_test::error;
// Work is published under a mutex and explicitly drained on another thread.
// No sleeps: each drain is a reproducible boundary in the operation's lifetime.
struct executor {
    std::mutex mutex;
    std::deque<task_wrapper_sbo> tasks;
    void dispatch(task_wrapper_sbo&& task) noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        tasks.emplace_back(std::move(task));
    }
    void drain() {
        for (;;) {
            task_wrapper_sbo task;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (tasks.empty()) break;
                task = std::move(tasks.front()); tasks.pop_front();
            }
            task();
        }
    }
    void on_worker() { std::thread worker([this] { drain(); }); worker.join(); }
};
enum class completion { inline_now, before_return, delayed, rejected };
enum class ordering { success, cancel_first, complete_first, race, abandon_control };
struct counter {
    std::atomic<int> value{0};
    void operator++() noexcept { value.fetch_add(1, std::memory_order_relaxed); }
    void operator--() noexcept { value.fetch_sub(1, std::memory_order_relaxed); }
    operator int() const noexcept { return value.load(std::memory_order_relaxed); }
};
struct counts {
    counter payloads, contexts, created, destroyed, canceled;
    counter delivered, values, errors, inspected;
};
struct context {
    counts* state;
    explicit context(counts* s) noexcept : state(s) { ++state->contexts; }
    ~context() { --state->contexts; }
};
struct payload {
    counts* state;
    int value;
    payload(counts* s, int v) noexcept : state(s), value(v) { ++state->payloads; }
    ~payload() { --state->payloads; }
};
struct backend { executor pending; completion mode; };
struct request {
    std::unique_ptr<payload> body;
    lite_ptr<context> session;
    backend* io;
};
using output = result_t<request, error>;

template <bool Fast> struct operation;
template <bool Fast> using operation_base = std::conditional_t<Fast,
    fast_awaitable_base<operation<Fast>, request, error>,
    awaitable_base<operation<Fast>, request, error>>;
template <bool Fast>
struct operation final : operation_base<Fast> {
    using async_result_type = output;
    request input;
    counts* state;
    static request take_request(output&& in) noexcept {
        // This test backend only accepts successful requests. Check the active
        // result alternative even in Release before moving its owned payload.
        if (!in.has_value()) {
            std::fputs("operation received an error instead of a request\n", stderr);
            std::abort();
        }
        return std::move(in.value());
    }
    explicit operation(output&& in) noexcept : input(take_request(std::move(in))), state(input.session->state) { ++state->created; }
    ~operation() { ++state->destroyed; }
    bool available() const noexcept { return true; }
    void cancel() noexcept { ++state->canceled; }
    int submit() noexcept {
        if (input.io->mode == completion::rejected) return -1;
        // Real backends can retain an operation beyond logical cancellation.
        this->retain();
        auto complete = [this]() noexcept {
            this->resume(output(value_tag, std::move(input)));
            this->release();
        };
        if (input.io->mode == completion::inline_now) complete();
        else if (input.io->mode == completion::before_return) {
            std::thread worker(complete); worker.join();
        } else input.io->pending.dispatch(task_wrapper_sbo(std::move(complete)));
        return 0;
    }
};
struct receiver {
    using value_type = output;
    counts* state;
    void emplace(output&& out) noexcept {
        ++state->delivered;
        if (out.has_value() && out.value().body && out.value().body->value == 42 &&
            out.value().session && state->contexts == 1) ++state->values;
        else if (out.has_error()) ++state->errors;
    }
};
struct start_gate {
    std::mutex mutex;
    std::condition_variable cv;
    int arrived = 0;
    void meet() {
        std::unique_lock<std::mutex> lock(mutex);
        if (++arrived == 2) cv.notify_all();
        else cv.wait(lock, [&] { return arrived == 2; });
    }
};

template <bool FastRunner, typename BP>
void launch(BP&& bp, request&& input, counts& state, lite_ptr<flow_controller> control, std::false_type) {
    auto ptr = make_lite_ptr<std::decay_t<BP>>(std::move(bp));
    flow_runner<std::decay_t<BP>, receiver> runner(ptr, std::move(control), receiver{&state});
    runner(std::move(input));
    // runner and the application's blueprint handle are dropped at return.
}
template <bool FastRunner, typename BP>
void launch(BP&& bp, request&& input, counts& state, lite_ptr<flow_controller>, std::true_type) {
    auto runner = make_fast_runner(std::move(bp), receiver{&state});
    runner(std::move(input));
}

template <bool FastRunner, bool FastAwaitable>
bool pipeline_case(completion mode, ordering order, bool hard) {
    counts state;
    executor first, second;
    backend io{{}, mode};
    auto control = make_lite_ptr<flow_controller>();
    auto bp = make_blueprint<request, error>()
        | position("request handoff", via(&first))
        | vaccine("request owns live session and payload", [&state](const output& out) noexcept {
            ++state.inspected;
            return out.has_error() || (out.value().body && out.value().session &&
                state.payloads == 1 && state.contexts == 1);
        })
        | await<operation<FastAwaitable>>(&second)
        | position("backend completion")
        | vaccine("completion is a live request or an error", [](const output& out) noexcept {
            return out.has_error() || (out.value().body && out.value().session && out.value().body->value == 42);
        }) | end();
    launch<FastRunner>(std::move(bp), request{std::unique_ptr<payload>(new payload(&state, 42)),
        make_lite_ptr<context>(&state), &io}, state, control, std::integral_constant<bool, FastRunner>{});
    bool ok = state.delivered == 0 && state.payloads == 1 && state.contexts == 1;
    first.on_worker();
    if (mode == completion::delayed) {
        ok = ok && state.destroyed == 0 && state.delivered == 0;
        if (order == ordering::abandon_control) control.release();
        if (order == ordering::cancel_first) {
            control->cancel(hard);
            second.on_worker();
            // Logical completion must not free resources still owned by the backend.
            ok = ok && state.destroyed == 0 && state.payloads == 1 && state.contexts == 1;
        }
        if (order == ordering::race) {
            start_gate gate;
            std::thread complete([&] { gate.meet(); io.pending.drain(); });
            std::thread cancel([&] { gate.meet(); control->cancel(hard); });
            complete.join(); cancel.join();
        } else io.pending.on_worker();
        if (order == ordering::complete_first) { second.on_worker(); control->cancel(hard); }
    }
    second.on_worker();
    control.release();
    const bool expected_error = mode == completion::rejected || order == ordering::cancel_first;
    ok = ok && state.created == 1 && state.destroyed == 1 && state.delivered == 1 &&
        state.values + state.errors == 1 && state.payloads == 0 && state.contexts == 0;
    if (order != ordering::race) ok = ok && state.errors == (expected_error ? 1 : 0);
    if (order == ordering::cancel_first) ok = ok && state.canceled == 1;
    std::printf("[%s] handoff fast-runner=%d fast-awaitable=%d mode=%d order=%d hard=%d\n",
        ok ? "OK" : "FAIL", FastRunner, FastAwaitable, int(mode), int(order), hard);
    return ok;
}

template <bool FastRunner, bool FastAwaitable>
bool basic_cases() {
    bool ok = true;
    for (auto mode : {completion::inline_now, completion::before_return, completion::delayed, completion::rejected})
        ok = pipeline_case<FastRunner, FastAwaitable>(mode, ordering::success, false) && ok;
    ok = pipeline_case<FastRunner, FastAwaitable>(completion::delayed, ordering::abandon_control, false) && ok;
    return ok;
}
template <typename... Args>
auto join_all(std::false_type, Args&&... args) { return await_when_all(std::forward<Args>(args)...); }
template <typename... Args>
auto join_all(std::true_type, Args&&... args) { return await_when_all_fast(std::forward<Args>(args)...); }
template <typename... Args>
auto join_any(std::false_type, Args&&... args) { return await_when_any(std::forward<Args>(args)...); }
template <typename... Args>
auto join_any(std::true_type, Args&&... args) { return await_when_any_fast(std::forward<Args>(args)...); }
using int_output = result_t<int, error>;
struct int_receiver {
    using value_type = int_output;
    counts* state;
    int* received;
    void emplace(int_output&& out) noexcept {
        ++state->delivered;
        if (out.has_value()) { ++state->values; *received = out.value(); }
        else ++state->errors;
    }
};
request make_request(counts& state, backend& io) {
    return {std::unique_ptr<payload>(new payload(&state, 42)), make_lite_ptr<context>(&state), &io};
}

template <bool Fast>
bool aggregate_case(bool any, bool reject, const int* order) {
    counts state;
    executor completed;
    backend io[3]{{{}, completion::delayed}, {{}, completion::delayed}, {{}, completion::delayed}};
    if (reject) io[1].mode = completion::rejected;
    int received = 0;
    {
        auto leaf = make_blueprint<request, error>() | await<operation<Fast>>()
            | transform([](request r) noexcept { return r.body->value; }) | end();
        auto ptr = make_lite_ptr<decltype(leaf)>(std::move(leaf));
        auto sum = [](int a, int b) noexcept { return int_output(value_tag, a + b); };
        auto failure = [](error e) noexcept { return int_output(error_tag, std::move(e)); };
        auto valid = [](const int_output& out) noexcept { return out.has_error() || out.value() > 0; };
        if (any) {
            auto bp = join_any(std::integral_constant<bool, Fast>{}, &completed,
                [](size_t, int v) noexcept { return int_output(value_tag, v); }, failure, ptr, ptr)
                | position("first result") | vaccine(valid) | end();
            auto runner = make_fast_runner(std::move(bp), int_receiver{&state, &received});
            runner(make_request(state, io[0]), make_request(state, io[1]));
        } else {
            auto inner = join_all(std::integral_constant<bool, Fast>{}, sum, failure, ptr, ptr) | end();
            auto inner_ptr = make_lite_ptr<decltype(inner)>(std::move(inner));
            auto bp = join_all(std::integral_constant<bool, Fast>{}, &completed, sum, failure, inner_ptr, ptr)
                | position("nested join") | vaccine(valid) | end();
            auto runner = make_fast_runner(std::move(bp), int_receiver{&state, &received});
            runner(make_flat_storage(make_request(state, io[0]), make_request(state, io[1])), make_request(state, io[2]));
        }
    } // Drop every public runner and blueprint handle while operations are pending.
    bool ok = state.delivered == 0;
    const int branches = any ? 2 : 3;
    for (int i = 0; i < branches; ++i) {
        io[order[i]].pending.on_worker();
        completed.on_worker();
        if (any && !reject && i == 0)
            ok = ok && state.delivered == 1 && state.payloads == 1 && state.destroyed == 1;
    }
    completed.on_worker();
    const bool expected_error = reject && !any;
    ok = ok && state.delivered == 1 && state.values + state.errors == 1 &&
        state.errors == (expected_error ? 1 : 0) && state.created == state.destroyed &&
        state.contexts == 0 && state.payloads == 0;
    if (!expected_error) ok = ok && received == (any ? 42 : 126);
    std::printf("[%s] aggregate fast=%d any=%d reject=%d order=%d%d%d created=%d destroyed=%d\n",
        ok ? "OK" : "FAIL", Fast, any, reject, order[0], order[1], order[2], int(state.created), int(state.destroyed));
    return ok;
}
} // namespace
int main(int argc, char** argv) {
    int repeats = argc > 1 ? std::atoi(argv[1]) : 1;
    if (repeats < 1 || repeats > 10000) return 2;
    bool ok = true;
    for (int repeat = 0; repeat < repeats; ++repeat) {
        std::printf("round=%d\n", repeat);
        ok = basic_cases<false, false>() && ok;
        ok = basic_cases<false, true>() && ok;
        ok = basic_cases<true, false>() && ok;
        ok = basic_cases<true, true>() && ok;
        for (bool hard : {false, true})
            for (auto order : {ordering::cancel_first, ordering::complete_first, ordering::race})
                ok = pipeline_case<false, false>(completion::delayed, order, hard) && ok;
        for (bool reject : {false, true}) {
            int order[3]{0, 1, 2};
            do {
                ok = aggregate_case<false>(false, reject, order) && ok;
                ok = aggregate_case<true>(false, reject, order) && ok;
            } while (std::next_permutation(order, order + 3));
            for (bool reversed : {false, true}) {
                int pair[3]{reversed ? 1 : 0, reversed ? 0 : 1, 2};
                ok = aggregate_case<false>(true, reject, pair) && ok;
                ok = aggregate_case<true>(true, reject, pair) && ok;
            }
        }
        if (!ok) return 1;
    }
    return 0;
}
