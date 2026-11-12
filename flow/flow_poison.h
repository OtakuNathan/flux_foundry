#ifndef FLUX_FOUNDRY_FLOW_POISON_H
#define FLUX_FOUNDRY_FLOW_POISON_H

#include "flow_contract.h"
#if FLUX_FOUNDRY_ENABLE_POISON
#include <mutex>
#include "../memory/lite_ptr.h"
#endif

namespace flux_foundry {
    enum class poison_failure { expired, duplicate_completion, rejected_completion,
        completion_then_rejection, missing_completion, not_quiescent,
        repeated_submission, allocation_failed };
    struct poison_violation {
        pipeline_position origin;
        pipeline_position binding;
        pipeline_position invalidated;
        poison_failure reason;
    };
    using poison_failure_hook = void (*)(const poison_violation&);
    inline const char* poison_failure_name(poison_failure reason) noexcept {
        switch (reason) {
        case poison_failure::expired: return "expired borrow or callback";
        case poison_failure::duplicate_completion: return "duplicate completion";
        case poison_failure::rejected_completion: return "completion after rejection";
        case poison_failure::completion_then_rejection: return "completion followed by rejection";
        case poison_failure::missing_completion: return "missing completion at quiescence";
        case poison_failure::not_quiescent: return "invalid quiescence boundary";
        case poison_failure::repeated_submission: return "submission result recorded twice";
        case poison_failure::allocation_failed: return "diagnostic allocation failed";
        }
        return "unknown violation";
    }
    inline void default_poison_failure_hook(const poison_violation& v) noexcept {
        std::fprintf(stderr, "flux_foundry poison: %s origin=%s (%s:%u) binding=%s (%s:%u) invalidated=%s (%s:%u)\n",
            poison_failure_name(v.reason), v.origin.name ? v.origin.name : "unknown", v.origin.file ? v.origin.file : "?", v.origin.line,
            v.binding.name ? v.binding.name : "unknown", v.binding.file ? v.binding.file : "?", v.binding.line,
            v.invalidated.name ? v.invalidated.name : "unknown", v.invalidated.file ? v.invalidated.file : "?", v.invalidated.line);
        std::fflush(stderr);
    }
    namespace flow_impl {
        [[noreturn]] inline void fail_poison(poison_violation v, poison_failure_hook hook) noexcept {
            (hook ? hook : default_poison_failure_hook)(v);
            std::abort();
        }
#if FLUX_FOUNDRY_ENABLE_POISON
        struct poison_state {
            std::mutex mutex;
            pipeline_position origin, invalidated{nullptr, nullptr, 0};
            poison_failure_hook hook;
            bool alive = true;
            poison_state(pipeline_position p, poison_failure_hook h) noexcept : origin(p), hook(h) {}
        };
        template <typename T, typename... Args>
        lite_ptr<T> poison_allocate(pipeline_position p, poison_failure_hook h, Args&&... args) noexcept {
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            try {
#endif
                auto ptr = make_lite_ptr<T>(std::forward<Args>(args)...);
                if (ptr) return ptr;
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            } catch (...) {}
#endif
            fail_poison({p, p, {}, poison_failure::allocation_failed}, h);
        }
#endif
    }
    class poison_token {
#if FLUX_FOUNDRY_ENABLE_POISON
        lite_ptr<flow_impl::poison_state> state;
        friend class poison_scope;
        explicit poison_token(lite_ptr<flow_impl::poison_state> s) noexcept : state(std::move(s)) {}
#endif
    public:
        poison_token() noexcept = default;
        void invalidate(pipeline_position p = {}) const noexcept {
#if FLUX_FOUNDRY_ENABLE_POISON
            if (!state) return;
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->alive) { state->invalidated = p; state->alive = false; }
#else
            (void)p;
#endif
        }
        void check(pipeline_position p = {}) const noexcept {
#if FLUX_FOUNDRY_ENABLE_POISON
            if (!state) return;
            bool alive;
            poison_violation failure;
            { std::lock_guard<std::mutex> lock(state->mutex);
                alive = state->alive;
                failure = {state->origin, p, state->invalidated, poison_failure::expired}; }
            if (!alive) flow_impl::fail_poison(failure, state->hook);
#else
            (void)p;
#endif
        }
        void fail(poison_failure reason, pipeline_position p) const noexcept {
#if FLUX_FOUNDRY_ENABLE_POISON
            poison_violation failure{p, p, {}, reason};
            if (state) {
                { std::lock_guard<std::mutex> lock(state->mutex);
                    failure.origin = state->origin; failure.invalidated = state->invalidated; }
                flow_impl::fail_poison(failure, state->hook);
            }
            flow_impl::fail_poison(failure, nullptr);
#else
            (void)reason; (void)p;
#endif
        }
    };
#if FLUX_FOUNDRY_ENABLE_POISON
    template <typename T> class poison_ref {
        T* ptr;
        poison_token token;
        pipeline_position binding;
    public:
        poison_ref(T& object, poison_token t, pipeline_position p) noexcept
            : ptr(std::addressof(object)), token(std::move(t)), binding(p) {}
        T& operator*() const noexcept { token.check(binding); return *ptr; }
        T* operator->() const noexcept { token.check(binding); return ptr; }
        poison_token lifetime_token() const noexcept { return token; }
    };
    template <typename T> poison_token poison_token_of(const poison_ref<T>& p) noexcept { return p.lifetime_token(); }
#else
    template <typename T> using poison_ref = T*;
    template <typename T> constexpr poison_token poison_token_of(T*) noexcept { return {}; }
#endif
    class poison_submission {
#if FLUX_FOUNDRY_ENABLE_POISON
        struct state_t {
            std::mutex mutex;
            poison_token lifetime;
            pipeline_position position;
            bool submitted = false, accepted = false, entered = false, finished = false, quiet = false;
            state_t(poison_token t, pipeline_position p) noexcept : lifetime(std::move(t)), position(p) {}
        };
        lite_ptr<state_t> state;
        template <typename F> struct callback {
            lite_ptr<state_t> state;
            F f;
            template <typename Fn, typename... Args>
            static void invoke(lite_ptr<state_t> local, Fn& f, Args&&... args) noexcept {
                static_assert(noexcept(f(std::forward<Args>(args)...)), "poison callback must be noexcept");
                static_assert(std::is_same<void, decltype(f(std::forward<Args>(args)...))>::value, "poison callback must return void");
                // Keep metadata alive even if f destroys the operation or this callback.
                bool bad = false;
                poison_failure reason = poison_failure::duplicate_completion;
                { std::lock_guard<std::mutex> lock(local->mutex);
                    if (local->entered) bad = true;
                    else if (local->submitted && !local->accepted) { bad = true; reason = poison_failure::rejected_completion; }
                    else if (local->quiet) { bad = true; reason = poison_failure::not_quiescent; }
                    else local->entered = true; }
                if (bad) local->lifetime.fail(reason, local->position);
                local->lifetime.check(local->position);
                f(std::forward<Args>(args)...);
                { std::lock_guard<std::mutex> lock(local->mutex); local->finished = true; }
            }
            template <typename... Args> void operator()(Args&&... args) noexcept {
                invoke(state, f, std::forward<Args>(args)...);
            }
            template <typename... Args>
            auto operator()(Args&&... args) const noexcept
                -> decltype(std::declval<const F&>()(std::forward<Args>(args)...)) {
                invoke(state, f, std::forward<Args>(args)...);
            }
        };
#endif
    public:
        poison_submission(poison_token t, pipeline_position p, poison_failure_hook h = nullptr) noexcept
#if FLUX_FOUNDRY_ENABLE_POISON
            : state(flow_impl::poison_allocate<state_t>(p, h, std::move(t), p))
#endif
        {
#if !FLUX_FOUNDRY_ENABLE_POISON
            (void)t; (void)p; (void)h;
#endif
        }
        template <typename F> auto wrap(F&& f) const noexcept {
            static_assert(flow_impl::require_nothrow_builder_storage<F>(), "invalid poison callback storage");
#if FLUX_FOUNDRY_ENABLE_POISON
            return callback<std::decay_t<F>>{state, std::forward<F>(f)};
#else
            return std::decay_t<F>(std::forward<F>(f));
#endif
        }
        int submitted(int code) const noexcept {
#if FLUX_FOUNDRY_ENABLE_POISON
            bool bad = false;
            poison_failure reason = poison_failure::repeated_submission;
            { std::lock_guard<std::mutex> lock(state->mutex);
                if (state->submitted || state->quiet) bad = true;
                else if (code && state->entered) { bad = true; reason = poison_failure::completion_then_rejection; }
                else { state->submitted = true; state->accepted = code == 0; } }
            if (bad) state->lifetime.fail(reason, state->position);
#endif
            return code;
        }
        void quiesce() const noexcept {
#if FLUX_FOUNDRY_ENABLE_POISON
            bool bad;
            poison_failure reason;
            { std::lock_guard<std::mutex> lock(state->mutex);
                bad = !state->submitted || (state->accepted && !state->finished);
                reason = state->entered || !state->submitted ? poison_failure::not_quiescent : poison_failure::missing_completion;
                state->quiet = true; }
            if (bad) state->lifetime.fail(reason, state->position);
#endif
        }
    };
    class poison_scope {
#if FLUX_FOUNDRY_ENABLE_POISON
        poison_token token_;
        pipeline_position origin;
        poison_failure_hook hook;
#endif
    public:
        explicit poison_scope(pipeline_position p = {}, poison_failure_hook h = nullptr) noexcept
#if FLUX_FOUNDRY_ENABLE_POISON
            : token_(flow_impl::poison_allocate<flow_impl::poison_state>(p, h, p, h)), origin(p), hook(h)
#endif
        {
#if !FLUX_FOUNDRY_ENABLE_POISON
            (void)p; (void)h;
#endif
        }
        poison_scope(const poison_scope&) = delete;
        poison_scope& operator=(const poison_scope&) = delete;
#if FLUX_FOUNDRY_ENABLE_POISON
        ~poison_scope() { token_.invalidate(origin); }
#endif
        poison_token token() const noexcept {
#if FLUX_FOUNDRY_ENABLE_POISON
            return token_;
#else
            return {};
#endif
        }
        void invalidate(pipeline_position p = {}) const noexcept { token().invalidate(p); }
        template <typename T> poison_ref<T> borrow(T& value, pipeline_position p = {}) const noexcept {
#if FLUX_FOUNDRY_ENABLE_POISON
            token_.check(p);
            return {value, token_, p};
#else
            (void)p; return std::addressof(value);
#endif
        }
        poison_submission submission(pipeline_position p = {}) const noexcept {
#if FLUX_FOUNDRY_ENABLE_POISON
            token_.check(p);
            return {token_, p, hook};
#else
            return {{}, p};
#endif
        }
    };
    namespace flow_impl {
        template <typename F> struct poison_node { const char* name; F selector; };
        struct disabled_poison_node {};
        template <typename F> struct poison_observer {
            pipeline_position position;
            F selector;
            template <typename R> void operator()(const R& r) const noexcept {
                static_assert(noexcept(selector(r)), "poison selector must be noexcept");
                static_assert(std::is_same<poison_token, decltype(selector(r))>::value, "poison selector must return poison_token");
                selector(r).invalidate(position);
            }
        };
        template <typename I, typename O, typename... Nodes, typename F>
        constexpr auto operator|(flow_blueprint<I, O, Nodes...>&& bp, poison_node<F>&& node) {
            static_assert(!is_runnable_bp_v<flow_blueprint<I, O, Nodes...>>, "poison must precede end()");
            static_assert(std::is_same<poison_token, decltype(std::declval<const F&>()(std::declval<const O&>()))>::value,
                "poison selector must accept const result_t& and return poison_token");
            static_assert(noexcept(std::declval<const F&>()(std::declval<const O&>())), "poison selector must be noexcept");
            auto p = preceding_position<0, sizeof...(Nodes)>::get(bp.nodes_);
            p.name = node.name;
            return std::move(bp) | flow_observer_node<O, poison_observer<F>>{{p, std::move(node.selector)}};
        }
        template <typename I, typename O, typename... Nodes>
        constexpr flow_blueprint<I, O, Nodes...>&& operator|(flow_blueprint<I, O, Nodes...>&& bp, disabled_poison_node) noexcept {
            static_assert(!is_runnable_bp_v<flow_blueprint<I, O, Nodes...>>, "poison must precede end()");
            return std::move(bp);
        }
    }
    template <typename F> constexpr auto poison(const char* name, F&& selector) noexcept {
#if FLUX_FOUNDRY_ENABLE_POISON
        static_assert(flow_impl::require_nothrow_builder_storage<F>(), "invalid poison selector storage");
        return flow_impl::poison_node<std::decay_t<F>>{name, std::forward<F>(selector)};
#else
        (void)name; (void)selector;
        return flow_impl::disabled_poison_node{};
#endif
    }
}

// Unlike a function returning F by value, the disabled spelling introduces no
// materialization or observable move of the callback expression (also in C++14).
// Variadic arguments allow lambda capture lists containing commas.
#if FLUX_FOUNDRY_ENABLE_POISON
# define FLUX_FOUNDRY_POISON_CALLBACK(check, ...) ((check).wrap(__VA_ARGS__))
#else
# define FLUX_FOUNDRY_POISON_CALLBACK(check, ...) (__VA_ARGS__)
#endif

#endif
