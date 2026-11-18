#include "lifecycle_test_support.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <type_traits>

using namespace flux_foundry;
namespace {
using error = lifecycle_test::error;
using output = result_t<int, error>;
enum class completion { inline_now, worker_before_return, delayed, rejected };

struct observations {
    int created = 0, destroyed = 0, canceled = 0;
    int delivered = 0, value = 0, errors = 0, marker_destroyed = 0, adapted = 0;
    std::size_t continuation_size = 0;
    bool continuation_sbo = false;
    completion mode = completion::delayed;
    task_wrapper_sbo terminal;
};

struct marker {
    observations* state;
    explicit marker(observations* s) noexcept : state(s) {}
    ~marker() noexcept { ++state->marker_destroyed; }
};

struct executor {
    task_wrapper_sbo pending;
    void dispatch(task_wrapper_sbo&& task) noexcept {
        if (pending) std::abort();
        pending = std::move(task);
    }
    void drain() noexcept {
        if (!pending) return;
        auto task = std::move(pending);
        task();
    }
};

struct request { observations* state; int value; };
using input = result_t<request, error>;

template <bool Fast> struct operation;
template <bool Fast> using operation_base = std::conditional_t<Fast,
    fast_awaitable_base<operation<Fast>, int, error>,
    awaitable_base<operation<Fast>, int, error>>;

template <bool Fast>
struct operation final : operation_base<Fast> {
    using base = operation_base<Fast>;
    using async_result_type = output;
    request in;
    explicit operation(input&& r) noexcept : in(r.value()) { ++in.state->created; }
    ~operation() noexcept { ++in.state->destroyed; }
    bool available() const noexcept { return true; }
    void cancel() noexcept { ++in.state->canceled; }

    // Observe the real continuation type before the ordinary delegate erases it.
    struct access_delegate : base::access_delegate {
        using parent = typename base::access_delegate;
        explicit access_delegate(parent p) noexcept : parent(p) {}
        template <typename F>
        void emplace_nextstep(F&& f) noexcept {
            auto* self = static_cast<operation*>(this->self);
            using closure = std::decay_t<F>;
            self->in.state->continuation_size = sizeof(closure);
            self->in.state->continuation_sbo = detail::can_enable_sbo<closure,
                callable_wrapper_sbo_size, alignof(std::max_align_t)>::value;
            parent::emplace_nextstep(std::forward<F>(f));
        }
    };
    access_delegate delegate() noexcept { return access_delegate(base::delegate()); }

    int submit() noexcept {
        const auto mode = in.state->mode;
        if (mode == completion::rejected) return -1;
        if (mode == completion::inline_now) {
            this->resume(output(value_tag, in.value));
            return 0;
        }
        this->retain(); // The backend remains alive if logical cancellation wins.
        auto finish = [this]() noexcept {
            this->resume(output(value_tag, in.value));
            this->release();
        };
        if (mode == completion::worker_before_return) {
            std::thread worker(finish);
            worker.join();
        } else {
            in.state->terminal = task_wrapper_sbo(std::move(finish));
        }
        return 0;
    }
};

struct receiver {
    using value_type = output;
    std::shared_ptr<observations> state;
    void emplace(output&& out) noexcept {
        ++state->delivered;
        if (out.has_value()) state->value = out.value();
        else ++state->errors;
    }
};

struct stateful_adaptor {
    std::shared_ptr<marker> keep_alive;
    int calls = 0;
    output operator()(output&& out) noexcept {
        ++keep_alive->state->adapted;
        ++calls; // Intentionally mutable: compression must retain concrete state.
        if (out.has_value()) return output(value_tag, out.value() + 6 + calls);
        return std::move(out);
    }
};

flow_impl::identity adaptor_for(const std::shared_ptr<marker>&, std::false_type) noexcept { return {}; }
stateful_adaptor adaptor_for(const std::shared_ptr<marker>& life, std::true_type) noexcept { return {life, 0}; }

template <typename BP>
lite_ptr<flow_controller> start(BP&& bp, const std::shared_ptr<observations>& state, std::false_type) {
    using blueprint = std::decay_t<BP>;
    auto runner = make_runner(make_lite_ptr<blueprint>(std::move(bp)), receiver{state});
    runner(request{state.get(), 35});
    return runner.get_controller();
}
template <typename BP>
lite_ptr<flow_controller> start(BP&& bp, const std::shared_ptr<observations>& state, std::true_type) {
    auto runner = make_fast_runner(std::move(bp), receiver{state});
    runner(request{state.get(), 35});
    return {};
}

template <bool FastRunner, bool FastAwaitable, bool Stateful, typename Executor>
bool run_case(Executor exec, executor& queue, completion mode, bool cancel_first) {
    using dispatcher = flow_impl::dispatch_wrapper_t<Executor>;
    auto state = std::make_shared<observations>();
    state->mode = mode;
    std::weak_ptr<marker> weak;
    lite_ptr<flow_controller> controller;
    {
        auto life = std::make_shared<marker>(state.get());
        weak = life;
        auto adaptor = adaptor_for(life, std::integral_constant<bool, Stateful>{});
        using node = flow_impl::flow_async_node<input, output, dispatcher, decltype(adaptor),
            awaitable_factory<operation<FastAwaitable>>>;
        auto bp = make_blueprint<request, error>()
            | transform([life](request r) noexcept { return r; })
            | node(dispatcher{exec}, std::move(adaptor), awaitable_factory<operation<FastAwaitable>>{})
            | end();
        controller = start(std::move(bp), state, std::integral_constant<bool, FastRunner>{});
    } // Only continuations/backend ownership may keep the blueprint alive now.
    bool ok = true;
    if (mode == completion::delayed || queue.pending) ok = !weak.expired();
    if (cancel_first) controller->cancel();
    if (state->terminal) {
        auto terminal = std::move(state->terminal);
        std::thread worker([&] { terminal(); });
        worker.join();
    }
    queue.drain();
    const bool failed = mode == completion::rejected || cancel_first;
    const int expected = Stateful ? 42 : 35;
    ok = ok && state->delivered == 1 && state->errors == (failed ? 1 : 0)
        && (failed || state->value == expected) && state->created == 1 && state->destroyed == 1
        && weak.expired() && state->marker_destroyed == 1;
    // Submission failure takes the runner error path without invoking the adaptor.
    if (Stateful) ok = ok && state->adapted == (mode == completion::rejected ? 0 : 1);
    const bool queued = !flow_impl::is_inline_dispatcher_v<dispatcher>;
    // Preserve the checked GCC/Clang non-MSVC layout as a size/SBO regression.
    // Pointer and receiver sizes alone do not identify MSVC's lambda/EBO layout.
    // Every ABI still runs the lifetime/state matrix and reports its actual SBO.
#if (defined(__GNUC__) || defined(__clang__)) && !defined(_MSC_VER)
    if (!FastRunner && !FastAwaitable && !Stateful && queued
            && sizeof(void*) == 8 && sizeof(receiver) == 16 && callable_wrapper_sbo_size == 48) {
        ok = ok && state->continuation_size == 48 && state->continuation_sbo;
    }
#endif
    std::printf("[%s] continuation runner=%d awaitable=%d stateful=%d queued=%d mode=%d cancel=%d bytes=%zu sbo=%d\n",
        ok ? "OK" : "FAIL", FastRunner, FastAwaitable, Stateful, queued,
        static_cast<int>(mode), cancel_first, state->continuation_size, state->continuation_sbo);
    return ok;
}

template <bool FastRunner, bool FastAwaitable, bool Stateful>
bool matrix() {
    bool ok = true;
    for (auto mode : {completion::inline_now, completion::worker_before_return,
                      completion::delayed, completion::rejected}) {
        executor q1, q2;
        ok = run_case<FastRunner, FastAwaitable, Stateful>(&q1, q1, mode, false) && ok;
        ok = run_case<FastRunner, FastAwaitable, Stateful>(flow_impl::inline_executor::executor(), q2, mode, false) && ok;
    }
    if (!FastRunner && !FastAwaitable) {
        executor q1, q2;
        ok = run_case<FastRunner, FastAwaitable, Stateful>(&q1, q1, completion::delayed, true) && ok;
        ok = run_case<FastRunner, FastAwaitable, Stateful>(flow_impl::inline_executor::executor(), q2, completion::delayed, true) && ok;
    }
    return ok;
}
} // namespace

int main() {
    bool ok = matrix<false, false, false>();
    ok = matrix<false, true, false>() && ok;
    ok = matrix<true, false, false>() && ok;
    ok = matrix<true, true, false>() && ok;
    ok = matrix<false, false, true>() && ok;
    ok = matrix<false, true, true>() && ok;
    ok = matrix<true, false, true>() && ok;
    ok = matrix<true, true, true>() && ok;
    return ok ? 0 : 1;
}
