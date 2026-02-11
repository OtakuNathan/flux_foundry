//
// Created by Nathan on 2/4/2026.
//

#ifndef FLUX_FOUNDRY_FLOW_ASYNC_AGGREGATOR_H
#define FLUX_FOUNDRY_FLOW_ASYNC_AGGREGATOR_H

#include <system_error>
#include <numeric>
#include <array>

#include "flow_awaitable.h"
#include "flow_runner.h"

namespace flux_foundry {
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    using flow_async_agg_err_t = std::exception_ptr;
#else
    using flow_async_agg_err_t = std::error_code;
#endif


namespace detail {
    static constexpr size_t launch_success_msk = (size_t(1) << 1);
    static constexpr size_t launch_failed_msk = (size_t(1) << 0);
    static constexpr size_t epoch = (size_t(1) << 2);

    static constexpr size_t successfully_finished = epoch | launch_success_msk;
    static constexpr size_t failed_finished = epoch | launch_failed_msk;
}

// when_all
template <typename Aggregator, typename... Ts>
struct flow_when_all_state {
    using storage_t = flat_storage<Ts...>;

    storage_t data;
    Aggregator* aggregator;
    // | 64 ... 2 | 1 |  0  |
    // | -------- | -- | --- | 
    // | fired runner | all fired? | failed to launch all runner? |
    std::atomic<size_t> fired;
    std::atomic<size_t> failed;
    std::array<lite_ptr<flow_controller>, sizeof...(Ts)> controllers;

    explicit flow_when_all_state(Aggregator* agg) noexcept
        : data{ Ts(error_tag, typename Ts::error_type {})... }
        , aggregator(agg), fired(0), failed(sizeof ... (Ts)) {
    }

    struct result_delegate {
        lite_ptr<flow_when_all_state> state;
        using value_type = storage_t;

        storage_t& get() noexcept {
            return state->data;
        }
    };
    
    using result_type = result_t<result_delegate, flow_async_agg_err_t>;

    template <size_t I>
    struct delegate {
    private:
        lite_ptr<flow_when_all_state> state;
    public:
        using value_type = flat_storage_element_t<I, storage_t>;

        delegate() = delete;
        explicit delegate(lite_ptr<flow_when_all_state> state_) noexcept
            : state(std::move(state_)) {
        }

        // Only call emplace once per delegate
        void emplace(value_type&& data) noexcept {
            value_type& e = get<I>(state->data);
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            try {
#endif
                e = std::move(data);
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            } catch (...) {
                e.emplace_error(std::current_exception());
            }
#endif

            UNLIKELY_IF(e.has_error()) {
                LIKELY_IF (state->fired.load(std::memory_order_acquire) & detail::launch_success_msk) {
                    for (size_t i = 0; i < sizeof ... (Ts); ++i) {
                        state->controllers[i]->cancel(true);
                    }
                } else {
                    for (size_t i = 0; i < I; ++i) {
                        state->controllers[i]->cancel(true);
                    }
                }

                size_t exp = sizeof...(Ts);
                state->failed.compare_exchange_strong(exp, I,  std::memory_order_release, std::memory_order_relaxed);
            }

            // last one is completed
            UNLIKELY_IF (state->fired.fetch_sub(detail::epoch, std::memory_order_release) == detail::successfully_finished) {
                std::atomic_thread_fence(std::memory_order_acquire);
                auto i = state->failed.load(std::memory_order_relaxed);
                LIKELY_IF (i == sizeof ... (Ts)) {
                    state->aggregator->resume(result_type(value_tag, result_delegate{state}));
                } else {
                    state->aggregator->resume(result_type(error_tag, async_any_failed_error<flow_async_agg_err_t>::make(i)));
                }
                state->aggregator->release();
            }
        }
    };
};

template <typename ... BPs>
struct flow_when_all_awaitable : 
    awaitable_base<flow_when_all_awaitable<BPs...>,
                   typename flow_when_all_state<flow_when_all_awaitable<BPs...>, typename BPs::O_t...>::result_delegate, /*T*/
                   flow_async_agg_err_t /*E*/> {
    static_assert(conjunction_v<flow_impl::is_runnable_bp<BPs>...>, "BPs should be runnable blue prints");

    using agg_t = flow_when_all_awaitable;
    using state_t = flow_when_all_state<flow_when_all_awaitable, typename BPs::O_t...>;
    using result_delegate = typename state_t::result_delegate;
    using result_type = typename state_t::result_type;

    constexpr static size_t N = sizeof ... (BPs);
private:

    template <size_t I>
    int launch() {
        auto on_error = [state = this->state_]() noexcept {
            // cancel all fired operations
            for (size_t i = 0; i < I; ++i) {
                state->controllers[i]->cancel(true);
            }

            // recall my self and set failed bit
            state->fired.fetch_sub(detail::epoch, std::memory_order_relaxed);
            state->fired.fetch_or(detail::launch_failed_msk, std::memory_order_acq_rel);
        };

#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        try {
#endif
            state_->fired.fetch_add(detail::epoch, std::memory_order_release);

            using pack_t   = flat_storage_element_t<I, bp_pack_storage_t>;
            using bp_ptr_t = typename pack_t::first_type;
            using bp_t     = typename bp_ptr_t::element_type;
            using receiver_t = typename state_t::template delegate<I>;
            auto controller = make_lite_ptr<flow_controller>();
#if !FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            UNLIKELY_IF (!controller) {
                on_error();
                return -1;
            }
#endif

            flow_runner<bp_t, receiver_t> runner(get<I>(this->packs).first(), controller, receiver_t(state_));
            state_->controllers[I] = runner.get_controller();
            runner(std::move(get<I>(this->packs).second()));
            return 0;
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        } catch (...) {
            on_error();
            throw;
        }
#endif
    }

public:
    using async_result_type = result_type;
    using bp_pack_storage_t = flat_storage<compressed_pair<lite_ptr<BPs>, std::decay_t<typename BPs::I_t::value_type>>...>;

    bp_pack_storage_t packs;
    lite_ptr<state_t> state_;
    
    template <typename ... Args,
#if FLUEX_FOUNDRY_HAS_EXCEPTIONS
        std::enable_if_t<std::is_constructible<bp_pack_storage_t, Args&&...>::value>* = nullptr
#else
        std::enable_if_t<std::is_nothrow_constructible<bp_pack_storage_t, Args&&...>::value>* = nullptr
#endif
    >
    explicit flow_when_all_awaitable(Args&& ... args)
#if !FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        noexcept(std::is_nothrow_constructible<bp_pack_storage_t, Args&&...>::value)
#endif
        : packs(std::forward<Args>(args)...)
        , state_(make_lite_ptr<state_t>(static_cast<agg_t*>(this))) {
    }
    
#if !FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    bool available() const noexcept {
        return static_cast<bool>(state_);
    }
#endif

    template <size_t ... idx>
    int submit(std::index_sequence<idx...>) {
        bool ok = true;
        using swallow = int[];
        (void)swallow {
            0, (ok && ((ok = (!launch<idx>() && this->state_->failed.load(std::memory_order_relaxed) == N))), 0)...
        };
        return ok ? 0 : -1;
    }

    // this should never be called at the same time with cancel. 
    int submit() noexcept {
        this->retain();
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        try {
#endif
            auto ret = this->submit(std::index_sequence_for<BPs...>{});
            UNLIKELY_IF (ret) {
                this->release();
                return -1;
            }

            // YOU MUST MARK ALL TASKS ARE SUBMITTED
            auto n = state_->fired.fetch_or(detail::launch_success_msk, std::memory_order_release);

            // all tasks are finished
            UNLIKELY_IF (n == 0) {
                std::atomic_thread_fence(std::memory_order_acquire);
                auto i = state_->failed.load(std::memory_order_relaxed);
                LIKELY_IF (i == sizeof ... (BPs)) {
                    this->resume(result_type(value_tag, result_delegate{state_}));
                } else {
                    this->resume(result_type(error_tag, async_any_failed_error<flow_async_agg_err_t>::make(i)));
                }
                this->release();
            }

#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        } catch (...) {
            // error occurred when launching the runner. no need to do anything, 
            // just release
            this->release();
            return -1;
        }
#endif
        return 0;
    }

    void cancel() noexcept {
        // error occurred, no need to cancel
        UNLIKELY_IF (state_->fired.load(std::memory_order_acquire) & detail::launch_failed_msk) {
            return;
        }

        for (size_t i = 0; i < state_->controllers.size(); ++i) {
            UNLIKELY_IF (state_->controllers[i]) {
                state_->controllers[i]->cancel(true);
            }
        }
    }
};

// when any
template <typename Aggregator, typename... Ts>
struct flow_when_any_state {
    using storage_t = flat_storage<Ts...>;
    storage_t data;
    Aggregator* aggregator;
    // | 64 ... 2 | 1 |  0  |
    // | -------- | -- | --- | 
    // | fired runner | all fired? | failed to launch all runner? |
    std::atomic<size_t> fired;
    std::atomic<size_t> winner;
    std::array<lite_ptr<flow_controller>, sizeof...(Ts)> controllers;

    explicit flow_when_any_state(Aggregator* agg) noexcept
        : data{ Ts(error_tag, typename Ts::error_type {})... }
        , aggregator(agg) , fired(0), winner(sizeof... (Ts)) {
    }

    struct result_delegate {
        lite_ptr<flow_when_any_state> state;
        using value_type = storage_t;

        size_t winner() noexcept {
            return state->winner.load(std::memory_order_acquire);
        }

        decltype(auto) get() noexcept {
            return state->data;
        }

        template <size_t I>
        decltype(auto) get_element() noexcept {
            return flux_foundry::get<I>(state->data);
        }
    };
    using result_type = result_t<result_delegate, flow_async_agg_err_t>;

    template <size_t I>
    struct delegate {
    private:
        using elem_type = flat_storage_element_t<I, storage_t>;
        lite_ptr<flow_when_any_state> state;
    public:
        using value_type = elem_type;

        delegate() = delete;
        explicit delegate(lite_ptr<flow_when_any_state> state_) noexcept
            : state(std::move(state_)) {
        }

        // Only call emplace once per delegate
        void emplace(elem_type&& data) noexcept {
            elem_type& e = get<I>(state->data);
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            try {
#endif
                e = std::move(data);
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            } catch (...) {
                e.emplace_error(std::current_exception());
            }
#endif
            bool im_winner = false;
            // fast success.
            LIKELY_IF(e.has_value()) {
                auto expected = sizeof...(Ts);
                LIKELY_IF(state->winner.compare_exchange_strong(expected, I,
                    std::memory_order_release, std::memory_order_relaxed)) {
                    im_winner = true;
                    LIKELY_IF(state->fired.load(std::memory_order_acquire) & detail::launch_success_msk) {
                        for (size_t i = 0; i < sizeof...(Ts); ++i) {
                            if (state->controllers[i]) {
                                state->controllers[i]->cancel(true);
                            }
                        }
                    } else {
                        for (size_t i = 0; i < I; ++i) {
                            if (state->controllers[i]) {
                                state->controllers[i]->cancel(true);
                            }
                        }
                    }
                    state->aggregator->resume(result_type(value_tag, result_delegate{state}));
                }
            }

            // If I am the last one
            UNLIKELY_IF (state->fired.fetch_sub(detail::epoch, std::memory_order_release) == detail::successfully_finished) {
                std::atomic_thread_fence(std::memory_order_acquire);
                if (!im_winner) {
                    // all failed?
                    UNLIKELY_IF(state->winner.load(std::memory_order_acquire) == sizeof...(Ts)) {
                        state->aggregator->resume(result_type(error_tag, async_all_failed_error<flow_async_agg_err_t>::make()));
                    }
                }
                state->aggregator->release();
            }
        }
    };
};

template <typename ... BPs>
struct flow_when_any_awaitable :
    awaitable_base<flow_when_any_awaitable<BPs...>,
               typename flow_when_any_state<flow_when_any_awaitable<BPs...>, typename BPs::O_t...>::result_delegate, /*T*/
               flow_async_agg_err_t /*E*/> {
    static_assert(conjunction_v<flow_impl::is_runnable_bp<BPs>...>, "BPs should be runnable blue prints");

    using agg_t   = flow_when_any_awaitable;
    using state_t = flow_when_any_state<agg_t, typename BPs::O_t...>;
    using result_delegate = typename state_t::result_delegate;
    using result_type = typename state_t::result_type;
    constexpr static size_t N = sizeof ... (BPs);

private:
    template <size_t I>
    int launch() {
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        try {
#endif
            state_->fired.fetch_add(detail::epoch, std::memory_order_release);

            using pack_t   = flat_storage_element_t<I, bp_pack_storage_t>;
            using bp_ptr_t = typename pack_t::first_type;
            using bp_t     = typename bp_ptr_t::element_type;
            using receiver_t = typename state_t::template delegate<I>;
            auto controller = make_lite_ptr<flow_controller>();
#if !FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            UNLIKELY_IF (!controller) {
                state_->fired.fetch_sub(detail::epoch, std::memory_order_acq_rel);
                return -1;
            }
#endif

            flow_runner<bp_t, receiver_t> runner(get<I>(this->packs).first(), controller, receiver_t(state_));
            state_->controllers[I] = runner.get_controller();
            runner(std::move(get<I>(this->packs).second()));
            return 0;
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        } catch (...) {
            state_->fired.fetch_sub(detail::epoch, std::memory_order_acq_rel);
            return -1;
        }
#endif
    }

public:
    using async_result_type = result_type;
    using bp_pack_storage_t = flat_storage<compressed_pair<lite_ptr<BPs>, std::decay_t<typename BPs::I_t::value_type>>...>;

    bp_pack_storage_t packs;
    lite_ptr<state_t> state_;
    
    template <typename ... Args,
#if FLUEX_FOUNDRY_HAS_EXCEPTIONS
        std::enable_if_t<std::is_constructible<bp_pack_storage_t, Args&&...>::value>* = nullptr
#else
        std::enable_if_t<std::is_nothrow_constructible<bp_pack_storage_t, Args&&...>::value>* = nullptr
#endif
    >
    explicit flow_when_any_awaitable(Args&& ... args)
#if !FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        noexcept(std::is_nothrow_constructible<bp_pack_storage_t, Args&&...>::value)
#endif
        : packs(std::forward<Args>(args)...)
        , state_(make_lite_ptr<state_t>(static_cast<agg_t*>(this))) {
    }

#if !FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    bool available() const noexcept {
        return static_cast<bool>(state_);
    }
#endif

    template <size_t ... idx>
    int submit(std::index_sequence<idx...>) {
        bool ok = true;
        int count = 0;
        using swallow = int[];
        (void)swallow {
            0, (ok && ((count += !launch<idx>()), ok = (this->state_->winner.load(std::memory_order_acquire) == N)), 0)...
        };
        return count;
    }

    // this should never be called at the same time with cancel. 
    int submit() noexcept {
        this->retain();
        auto ret = this->submit(std::index_sequence_for<BPs...>{});
        // all failed when submitting.
        UNLIKELY_IF (ret == 0) {
            state_->fired.fetch_or(detail::launch_failed_msk, std::memory_order_release);
            this->release();
            return -1;
        }

        // YOU MUST MARK ALL TASKS ARE SUBMITTED
        auto n = state_->fired.fetch_or(detail::launch_success_msk, std::memory_order_release);

        // all tasks are finished
        UNLIKELY_IF(n == 0) {
            std::atomic_thread_fence(std::memory_order_acquire);
            UNLIKELY_IF (state_->winner.load(std::memory_order_relaxed) == N) {
                state_->aggregator->resume(result_type(error_tag, async_all_failed_error<flow_async_agg_err_t>::make()));
            }
            this->release();
        }

        return 0;
    }

    void cancel() noexcept {
        // error occurred, no need to cancel
        UNLIKELY_IF (state_->fired.load(std::memory_order_acquire) & detail::launch_failed_msk) {
            return;
        }

        for (size_t i = 0; i < state_->controllers.size(); ++i) {
            UNLIKELY_IF (state_->controllers[i]) {
                state_->controllers[i]->cancel(true);
            }
        }
    }
};

template <typename awaitable, typename ... BPs>
struct aggregator_awaitable_factory {
    static_assert(conjunction_v<flow_impl::is_blueprint<BPs>...>, "BPs should be blueprints");

    template <typename U>
    constexpr static auto detect_submit_async(int)
        -> conjunction<std::is_convertible<decltype(std::declval<U&>().submit()), int>,
            std::integral_constant<bool, noexcept(std::declval<U&>().submit())>>;

    template <typename U>
    constexpr static auto detect_submit_async(...) -> std::false_type;

    static_assert(decltype(detect_submit_async<awaitable>(0))::value,
        "awaitable requirement: Missing or invalid 'submit'.\n"
        "Expected signature: int submit() noexcept.");

    template <typename U>
    constexpr static auto detect_cancel(int) -> std::integral_constant<bool, noexcept(std::declval<U&>().cancel())>;

    template <typename U>
    constexpr static auto detect_cancel(...) -> std::false_type;

    static_assert(decltype(detect_cancel<awaitable>(0))::value,
        "awaitable requirement: Missing 'void cancel() noexcept'.\n"
        "Must be able to cancel pending async operation and release resources.");

#if !FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    template <typename U>
    constexpr static auto detect_available(int)
        -> conjunction<std::is_convertible<decltype(std::declval<U&>().available()), int>,
            std::integral_constant<bool, noexcept(std::declval<U&>().available())>>;

    template <typename U>
    constexpr static auto detect_available(...) -> std::false_type;

    static_assert(decltype(detect_available<awaitable>(0))::value,
        "awaitable requirement: Missing 'bool available() noexcept'.\n"
        "Must be able to provide whether the awaitable is fully created and initialized.");
#endif

    static_assert(is_awaitable_v<awaitable>,
        "the providing type Awaitable must can be an class that inherits awaitable_base.");

    using node_error_t = typename awaitable::async_result_type::error_type;
    using awaitable_t = awaitable;

    flat_storage<lite_ptr<BPs>...> bps;

    explicit aggregator_awaitable_factory(lite_ptr<BPs> ... bps_) noexcept
        : bps(std::move(bps_)...) {
    }

    template <size_t... I, typename ... Args>
    auto create_awaitable(std::index_sequence<I...>, flat_storage<Args...>&& params) {
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        return new awaitable(make_compressed_pair(get<I>(this->bps), std::move(get<I>(params)))...);
#else
        return new (std::nothrow) awaitable(make_compressed_pair(get<I>(this->bps), std::move(get<I>(params)))...);
#endif
    }

    template <typename A = awaitable, typename ... Args,
#if FLUEX_FOUNDRY_HAS_EXCEPTIONS
        std::enable_if_t<std::is_constructible<A, compressed_pair<lite_ptr<BPs>, Args>...>::value>* = nullptr
#else
        std::enable_if_t<std::is_nothrow_constructible<A, compressed_pair<lite_ptr<BPs>, std::decay_t<Args>>...>::value>* = nullptr
#endif
    >
    result_t<typename awaitable::access_delegate, node_error_t>
        operator()(result_t<flat_storage<Args...>, flow_async_agg_err_t>&& params) noexcept {
        static_assert(sizeof...(Args) == sizeof...(BPs), "Input parameters count mismatch");

        UNLIKELY_IF (!params.has_value()) {
            return result_t<typename awaitable::access_delegate, node_error_t>(error_tag, params.error());
        }

#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        try {
            auto aw = create_awaitable(std::index_sequence_for<BPs...> {}, std::move(params.value()));
            return result_t<typename awaitable::access_delegate, node_error_t>(value_tag, aw->delegate());
        } catch (...) {
            return result_t<typename awaitable::access_delegate, node_error_t>(error_tag, std::current_exception());
        }
#else
        auto aw = create_awaitable(std::index_sequence_for<BPs...> {}, std::move(params.value()));
        UNLIKELY_IF(!aw)  {
            return result_t<typename awaitable::access_delegate, node_error_t>(error_tag,
                awaitable_creating_error<node_error_t>::make());
        }

        UNLIKELY_IF(!aw->available()) {
            aw->release();
            return result_t<typename awaitable::access_delegate, node_error_t>(error_tag,
                awaitable_creating_error<node_error_t>::make());
        }

        return result_t<typename awaitable::access_delegate, node_error_t>(value_tag, aw->delegate());
#endif
    }
};

}


#endif //FLUX_FOUNDRY_FLOW_ASYNC_AGGREGATOR_H
