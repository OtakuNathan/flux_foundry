//
// Created by Nathan on 2/4/2026.
//

#ifndef FLUX_FOUNDRY_FLOW_ASYNC_AGGREGATOR_H
#define FLUX_FOUNDRY_FLOW_ASYNC_AGGREGATOR_H

#include <system_error>
#include <numeric>
#include <array>
#include <cstdint>
#include <type_traits>

#include "../memory/padded_t.h"
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

    template <typename Awaitable>
    struct intrusive_awaitable_ptr {
        Awaitable* p;

        intrusive_awaitable_ptr() noexcept
            : p(nullptr) {
        }

        explicit intrusive_awaitable_ptr(Awaitable* p_) noexcept
            : p(p_) {
            if (p) {
                p->retain();
            }
        }

        intrusive_awaitable_ptr(const intrusive_awaitable_ptr& rhs) noexcept
            : p(rhs.p) {
            if (p) {
                p->retain();
            }
        }

        intrusive_awaitable_ptr(intrusive_awaitable_ptr&& rhs) noexcept
            : p(rhs.p) {
            rhs.p = nullptr;
        }

        intrusive_awaitable_ptr& operator=(const intrusive_awaitable_ptr& rhs) noexcept {
            if (this != &rhs) {
                reset();
                p = rhs.p;
                if (p) {
                    p->retain();
                }
            }
            return *this;
        }

        intrusive_awaitable_ptr& operator=(intrusive_awaitable_ptr&& rhs) noexcept {
            if (this != &rhs) {
                reset();
                p = rhs.p;
                rhs.p = nullptr;
            }
            return *this;
        }

        ~intrusive_awaitable_ptr() noexcept {
            reset();
        }

        void reset() noexcept {
            if (p) {
                p->release();
                p = nullptr;
            }
        }

        Awaitable* get() const noexcept {
            return p;
        }

        Awaitable* operator->() const noexcept {
            return p;
        }

        explicit operator bool() const noexcept {
            return p != nullptr;
        }
    };
}

// when_all state
template <typename Awaitable, typename... Ts>
struct flow_when_all_state {
    using state_t = flow_when_all_state;
    using storage_t = flat_storage<Ts...>;
    using owner_ptr_t = detail::intrusive_awaitable_ptr<Awaitable>;
    using controller_ptr_t = flow_controller*;

    storage_t data;
    // | 64 ... 2 | 1 |  0  |
    // | -------- | -- | --- |
    // | fired runner | all fired? | failed to launch all runner? |
    padded_t<std::atomic<size_t>, CACHE_LINE_SIZE> fired;
    padded_t<std::atomic<size_t>, CACHE_LINE_SIZE> failed;
    std::array<flow_controller, sizeof...(Ts)> controllers;

    flow_when_all_state() noexcept
        : data{ Ts(error_tag, typename Ts::error_type {})... }
        , fired(0) , failed(sizeof...(Ts)), controllers{} {
    }

    struct result_delegate {
        owner_ptr_t owner;
        using value_type = storage_t;

        explicit  result_delegate(Awaitable* owner_) noexcept
            : owner(owner_) { }

        explicit result_delegate(const owner_ptr_t& owner_) noexcept
            : owner(owner_) { }

        storage_t& get() noexcept {
            return owner->state_.data;
        }
    };

    using result_type = result_t<result_delegate, flow_async_agg_err_t>;

    template <size_t I>
    struct delegate {
    private:
        owner_ptr_t owner;

    public:
        using value_type = flat_storage_element_t<I, storage_t>;

        delegate() = delete;

        explicit delegate(Awaitable* owner_) noexcept
            : owner(owner_) {
        }

        void emplace(value_type&& data_) noexcept {
            Awaitable* owner_raw = owner.get();
            auto& state = owner_raw->state_;
            value_type& e = get<I>(state.data);
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            try {
#endif
                e = std::move(data_);
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            } catch (...) {
                e.emplace_error(std::current_exception());
            }
#endif
            UNLIKELY_IF (e.has_error()) {
                for (size_t i = 0; i < sizeof...(Ts); ++i) {
                    state.controllers[i].cancel(true);
                }

                size_t exp = sizeof...(Ts);
                state.failed.get().compare_exchange_strong(exp, I,
                    std::memory_order_release, std::memory_order_relaxed);
            }

#if FLUEX_FOUNDRY_WITH_TSAN
            UNLIKELY_IF (state.fired.get().fetch_sub(detail::epoch, std::memory_order_acq_rel) == detail::successfully_finished) {
#else
            UNLIKELY_IF (state.fired.get().fetch_sub(detail::epoch, std::memory_order_release) == detail::successfully_finished) {
                std::atomic_thread_fence(std::memory_order_acquire);
#endif
                auto i = state.failed.get().load(std::memory_order_relaxed);
                LIKELY_IF (i == sizeof...(Ts)) {
                    owner_raw->resume(result_type(value_tag, typename state_t::result_delegate(owner)));
                } else {
                    owner_raw->resume(result_type(error_tag, async_any_failed_error<flow_async_agg_err_t>::make(i)));
                }
            }
        }
    };
};

template <typename... BPs>
struct flow_when_all_awaitable final :
        awaitable_base<flow_when_all_awaitable<BPs...>,
            typename flow_when_all_state<flow_when_all_awaitable<BPs...>, typename BPs::O_t...>::result_delegate,
            flow_async_agg_err_t> {
    static_assert(conjunction_v<flow_impl::is_runnable_bp<BPs>...>, "BPs should be runnable blue prints");

    constexpr static size_t N = sizeof...(BPs);
    using agg_t = flow_when_all_awaitable;
    using state_t = flow_when_all_state<agg_t, typename BPs::O_t...>;
    using result_delegate = typename state_t::result_delegate;
    using result_type = typename state_t::result_type;
    using bp_pack_storage_t = flat_storage<compressed_pair<lite_ptr<BPs>, std::decay_t<typename BPs::I_t::value_type>>...>;

    using async_result_type = result_type;

    bp_pack_storage_t packs;
    state_t state_;

    template <size_t... idx>
    bool all_bps_present(std::index_sequence<idx...>) const noexcept {
        bool ok = true;
        using swallow = int[];
        (void)swallow{
            0, ((ok = (ok && static_cast<bool>(get<idx>(packs).first()))), 0)...
        };
        return ok;
    }

    template <size_t I>
    int launch() {
        auto on_error = [this]() noexcept {
            for (size_t i = 0; i < I; ++i) {
                state_.controllers[i].cancel(true);
            }
            state_.fired.get().fetch_sub(detail::epoch, std::memory_order_relaxed);
            state_.fired.get().fetch_or(detail::launch_failed_msk, std::memory_order_acq_rel);
        };

#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        try {
#endif
            state_.fired.get().fetch_add(detail::epoch, std::memory_order_release);

            using pack_t = flat_storage_element_t<I, bp_pack_storage_t>;
            using bp_ptr_t = typename pack_t::first_type;
            using bp_t = typename bp_ptr_t::element_type;
            using receiver_t = typename state_t::template delegate<I>;
            auto controller = &state_.controllers[I];

            using runner_t = flow_runner<bp_t, receiver_t, decltype(controller)>;
            runner_t runner(get<I>(this->packs).first(), controller, receiver_t(this));
            runner(std::move(get<I>(this->packs).second()));
            return 0;
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        } catch (...) {
            on_error();
            throw;
        }
#endif
    }

    template <typename... Args,
#if FLUEX_FOUNDRY_HAS_EXCEPTIONS
        std::enable_if_t<std::is_constructible<bp_pack_storage_t, Args&&...>::value>* = nullptr
#else
        std::enable_if_t<std::is_nothrow_constructible<bp_pack_storage_t, Args&&...>::value>* = nullptr
#endif
    >
    explicit flow_when_all_awaitable(Args&&... args)
#if !FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        noexcept(std::is_nothrow_constructible<bp_pack_storage_t, Args&&...>::value)
#endif
        : packs(std::forward<Args>(args)...), state_() {
    }

#if !FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    bool available() const noexcept {
        return true;
    }
#endif

    template <size_t... idx>
    int submit(std::index_sequence<idx...>) {
        bool ok = true;
        using swallow = int[];
        (void)swallow {
            0, (ok && ((ok = !launch<idx>()) && (state_.failed.get().load(std::memory_order_relaxed) == N)), 0)...
        };
        return ok ? 0 : -1;
    }

    int submit() noexcept {
        // when_all requires every blueprint to be present.
        UNLIKELY_IF (!all_bps_present(std::index_sequence_for<BPs...>{})) {
            return -1;
        }

#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        try {
#endif
            auto ret = this->submit(std::index_sequence_for<BPs...>{});
            UNLIKELY_IF (ret) {
                return -1;
            }

#if FLUEX_FOUNDRY_WITH_TSAN
            UNLIKELY_IF (state_.fired.get().fetch_or(detail::launch_success_msk, std::memory_order_acq_rel) == 0) {
#else
            UNLIKELY_IF (state_.fired.get().fetch_or(detail::launch_success_msk, std::memory_order_release) == 0) {
                std::atomic_thread_fence(std::memory_order_acquire);
#endif
                auto i = state_.failed.get().load(std::memory_order_relaxed);
                LIKELY_IF (i == N) {
                    this->resume(result_type(value_tag, result_delegate(this)));
                } else {
                    this->resume(result_type(error_tag, async_any_failed_error<flow_async_agg_err_t>::make(i)));
                }
            }

#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        } catch (...) {
            return -1;
        }
#endif
        return 0;
    }

    void cancel() noexcept {
        UNLIKELY_IF (state_.fired.get().load(std::memory_order_acquire) & detail::launch_failed_msk) {
            return;
        }

        for (size_t i = 0; i < N; ++i) {
            state_.controllers[i].cancel(true);
        }
    }
};

template <typename Awaitable, typename... Ts>
struct flow_when_all_fast_state {
    using state_t = flow_when_all_fast_state;
    using storage_t = flat_storage<Ts...>;
    using owner_ptr_t = detail::intrusive_awaitable_ptr<Awaitable>;

    storage_t data;
    // | 64 ... 2 | 1 |  0  |
    // | -------- | -- | --- |
    // | fired runner | all fired? | failed to launch all runner? |
    padded_t<std::atomic<size_t>, CACHE_LINE_SIZE> fired;
    padded_t<std::atomic<size_t>, CACHE_LINE_SIZE> failed;

    flow_when_all_fast_state() noexcept
        : data{ Ts(error_tag, typename Ts::error_type {})... }
        , fired(0) , failed(sizeof...(Ts)) {
    }

    // result output
    struct result_delegate {
        owner_ptr_t owner;
        using value_type = storage_t;

        explicit  result_delegate(Awaitable* owner_) noexcept
            : owner(owner_) { }

        explicit result_delegate(const owner_ptr_t& owner_) noexcept
            : owner(owner_) { }

        storage_t& get() noexcept {
            return owner->state_.data;
        }
    };

    using result_type = result_t<result_delegate, flow_async_agg_err_t>;

    template <size_t I>
    struct delegate {
    private:
        owner_ptr_t owner;

    public:
        using value_type = flat_storage_element_t<I, storage_t>;

        explicit delegate(Awaitable* owner_) noexcept
            : owner(owner_) {
        }

        void emplace(value_type&& data_) noexcept {
            Awaitable *owner_raw = owner.get();
            auto &state = owner_raw->state_;
            value_type &e = get<I>(state.data);
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            try {
#endif
                e = std::move(data_);
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            } catch (...) {
                e.emplace_error(std::current_exception());
            }
#endif
            UNLIKELY_IF (e.has_error()) {
                size_t exp = sizeof...(Ts);
                state.failed.get().compare_exchange_strong(exp, I, std::memory_order_acq_rel, std::memory_order_relaxed);
            }

#if FLUEX_FOUNDRY_WITH_TSAN
            UNLIKELY_IF (state.fired.get().fetch_sub(detail::epoch, std::memory_order_acq_rel) == detail::successfully_finished) {
#else
            UNLIKELY_IF (state.fired.get().fetch_sub(detail::epoch, std::memory_order_release) == detail::successfully_finished) {
                std::atomic_thread_fence(std::memory_order_acquire);
#endif
                auto i = state.failed.get().load(std::memory_order_relaxed);
                LIKELY_IF (i == sizeof...(Ts)) {
                    owner_raw->resume(result_type(value_tag, typename state_t::result_delegate(owner)));
                } else {
                    owner_raw->resume(result_type(error_tag, async_any_failed_error<flow_async_agg_err_t>::make(i)));
                }
            }
        }
    };
};

template <typename ... BPs>
struct flow_when_all_fast_awaitable final :
        fast_awaitable_base<flow_when_all_fast_awaitable<BPs...>,
            typename flow_when_all_fast_state<flow_when_all_fast_awaitable<BPs...>, typename BPs::O_t...>::result_delegate,
            flow_async_agg_err_t> {
    static_assert(conjunction_v<flow_impl::is_runnable_bp<BPs>...>, "BPs should be runnable blue prints");

    using agg_t = flow_when_all_fast_awaitable;
    using state_t = flow_when_all_fast_state<agg_t, typename BPs::O_t...>;
    using result_delegate = typename state_t::result_delegate;
    using result_type = typename state_t::result_type;
    constexpr static size_t N = sizeof...(BPs);
    using async_result_type = result_type;
    using bp_pack_storage_t = flat_storage<compressed_pair<lite_ptr<BPs>, std::decay_t<typename BPs::I_t::value_type>>...>;

    bp_pack_storage_t packs;
    state_t state_;

    template<size_t... idx>
    bool all_bps_present(std::index_sequence<idx...>) const noexcept {
        bool ok = true;
        using swallow = int[];
        (void) swallow{
            0, ((ok = (ok && static_cast<bool>(get<idx>(packs).first()))), 0)...
        };
        return ok;
    }

    template<size_t I>
    int launch() {
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        try {
#endif
            state_.fired.get().fetch_add(detail::epoch, std::memory_order_release);

            using receiver_t = typename state_t::template delegate<I>;
            auto runner = make_fast_runner(get<I>(this->packs).first(), receiver_t(this));
            runner(std::move(get<I>(this->packs).second()));
            return 0;
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        } catch (...) {
            state_.fired.get().fetch_sub(detail::epoch, std::memory_order_acq_rel);
            throw;
        }
#endif
    }

    template <typename... Args,
#if FLUEX_FOUNDRY_HAS_EXCEPTIONS
            std::enable_if_t<std::is_constructible<bp_pack_storage_t, Args&&...>::value>* = nullptr
#else
            std::enable_if_t<std::is_nothrow_constructible<bp_pack_storage_t, Args&&...>::value>* = nullptr
#endif
    >
    explicit flow_when_all_fast_awaitable(Args&&... args)
#if !FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    noexcept(std::is_nothrow_constructible<bp_pack_storage_t, Args&&...>::value)
#endif
            : packs(std::forward<Args>(args)...), state_() {
    }

#if !FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    bool available() const noexcept {
        return true;
    }
#endif

    template <size_t... idx>
    int submit(std::index_sequence<idx...>) {
        bool ok = true;
        using swallow = int[];
        (void)swallow {
            0, (ok && ((ok = !launch<idx>()) && (state_.failed.get().load(std::memory_order_relaxed) == N)), 0)...
        };
        return ok ? 0 : -1;
    }

    int submit() noexcept {
        // when_all requires every blueprint to be present.
        UNLIKELY_IF (!all_bps_present(std::index_sequence_for<BPs...>{})) {
            return -1;
        }

#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        try {
#endif
            auto ret = this->submit(std::index_sequence_for<BPs...>{});
            UNLIKELY_IF (ret) {
                return -1;
            }

#if FLUEX_FOUNDRY_WITH_TSAN
            UNLIKELY_IF (state_.fired.get().fetch_or(detail::launch_success_msk, std::memory_order_acq_rel) == 0) {
#else
            UNLIKELY_IF (state_.fired.get().fetch_or(detail::launch_success_msk, std::memory_order_release) == 0) {
                std::atomic_thread_fence(std::memory_order_acquire);
#endif
                auto i = state_.failed.get().load(std::memory_order_relaxed);
                LIKELY_IF (i == N) {
                    this->resume(result_type(value_tag, result_delegate(this)));
                } else {
                    this->resume(result_type(error_tag, async_any_failed_error<flow_async_agg_err_t>::make(i)));
                }
            }
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        } catch (...) {
            return -1;
        }
#endif
        return 0;
    }

    void cancel() noexcept {}
};

// when_any state
template <typename Awaitable, typename... Ts>
struct flow_when_any_state {
    using state_t = flow_when_any_state;
    using storage_t = flat_storage<Ts...>;
    using owner_ptr_t = detail::intrusive_awaitable_ptr<Awaitable>;
    using controller_ptr_t = flow_controller*;

    storage_t data;
    // | 64 ... 2 | 1 |  0  |
    // | -------- | -- | --- |
    // | fired runner | all fired? | failed to launch all runner? |
    padded_t<std::atomic<size_t>, CACHE_LINE_SIZE> fired;
    padded_t<std::atomic<size_t>, CACHE_LINE_SIZE> winner;
    std::array<flow_controller, sizeof...(Ts)> controllers;

    flow_when_any_state() noexcept
        : data{ Ts(error_tag, typename Ts::error_type {})... }
        , fired(0)
        , winner(sizeof...(Ts))
        , controllers{} {
    }

    struct result_delegate {
        owner_ptr_t owner;
        using value_type = storage_t;

        result_delegate() noexcept
            : owner() { }

        explicit result_delegate(const owner_ptr_t& owner_) noexcept
            : owner(owner_) { }

        explicit result_delegate(Awaitable* owner_) noexcept
            : owner(owner_) { }

        size_t winner() noexcept {
            return owner->state_.winner.get().load(std::memory_order_acquire);
        }

        storage_t& get() noexcept {
            return owner->state_.data;
        }
    };

    using result_type = result_t<result_delegate, flow_async_agg_err_t>;

    template <size_t I>
    struct delegate {
    private:
        owner_ptr_t owner;

    public:
        using value_type = flat_storage_element_t<I, storage_t>;

        delegate() = delete;

        explicit delegate(Awaitable* owner_) noexcept
            : owner(owner_) {
        }

        void emplace(value_type&& data_) noexcept {
            auto* owner_raw = owner.get();
            auto& state = owner_raw->state_;
            value_type& e = get<I>(state.data);
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            try {
#endif
                e = std::move(data_);
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            } catch (...) {
                e.emplace_error(std::current_exception());
            }
#endif
            bool i_won = false;
            LIKELY_IF (e.has_value()) {
                auto expected = sizeof...(Ts);
                LIKELY_IF (state.winner.get().compare_exchange_strong(expected, I,
                    std::memory_order_release, std::memory_order_relaxed)) {
                    i_won = true;

                    owner_raw->resume(result_type(value_tag, typename state_t::result_delegate(owner)));
                    for (size_t i = 0; i < sizeof...(Ts); ++i) {
                        state.controllers[i].cancel(true);
                    }
                }
            }

#if FLUEX_FOUNDRY_WITH_TSAN
            UNLIKELY_IF (state.fired.get().fetch_sub(detail::epoch, std::memory_order_acq_rel) == detail::successfully_finished) {
#else
            UNLIKELY_IF (state.fired.get().fetch_sub(detail::epoch, std::memory_order_release) == detail::successfully_finished) {
                std::atomic_thread_fence(std::memory_order_acquire);
#endif
                // nobody won and I am the last one. I have to tell the suspending runner we all failed.
                UNLIKELY_IF (!i_won && state.winner.get().load(std::memory_order_acquire) == sizeof...(Ts)) {
                    owner_raw->resume(result_type(error_tag, async_all_failed_error<flow_async_agg_err_t>::make()));
                    return;
                }
            }
        }
    };
};

template <typename Awaitable, typename... Ts>
struct flow_when_any_fast_state  {
    using state_t = flow_when_any_fast_state;
    using storage_t = flat_storage<Ts...>;
    using owner_ptr_t = detail::intrusive_awaitable_ptr<Awaitable>;

    storage_t data;
    // | 64 ... 2 | 1 |  0  |
    // | -------- | -- | --- |
    // | fired runner | all fired? | failed to launch all runner? |
    padded_t<std::atomic<size_t>, CACHE_LINE_SIZE> fired;
    padded_t<std::atomic<size_t>, CACHE_LINE_SIZE> winner;

    flow_when_any_fast_state() noexcept
        : data{ Ts(error_tag, typename Ts::error_type {})... }
        , fired(0), winner(sizeof...(Ts)) {
    }

    struct result_delegate {
        owner_ptr_t owner;
        using value_type = storage_t;

        explicit result_delegate(const owner_ptr_t& owner_) noexcept
            : owner(owner_) { }

        explicit result_delegate(Awaitable* owner_) noexcept
            : owner(owner_) { }

        size_t winner() noexcept {
            return owner->state_.winner.get().load(std::memory_order_acquire);
        }

        storage_t& get() noexcept {
            return owner->state_.data;
        }
    };

    using result_type = result_t<result_delegate, flow_async_agg_err_t>;

    template <size_t I>
    struct delegate {
    private:
        owner_ptr_t owner;

    public:
        using value_type = flat_storage_element_t<I, storage_t>;

        delegate() = delete;

        explicit delegate(Awaitable* owner_) noexcept
            : owner(owner_) {
        }

        void emplace(value_type&& data_) noexcept {
            auto* owner_raw = owner.get();
            auto& state = owner_raw->state_;
            value_type& e = get<I>(state.data);
            bool i_won = false;
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            try {
#endif
                e = std::move(data_);
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            } catch (...) {
                e.emplace_error(std::current_exception());
            }
#endif
            LIKELY_IF (e.has_value()) {
                auto expected = sizeof...(Ts);
                LIKELY_IF (state.winner.get().compare_exchange_strong(expected, I,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                    i_won = true;
                    owner_raw->resume(result_type(value_tag, typename state_t::result_delegate(owner)));
                }
            }

#if FLUEX_FOUNDRY_WITH_TSAN
            UNLIKELY_IF (state.fired.get().fetch_sub(detail::epoch, std::memory_order_acq_rel) == detail::successfully_finished) {
#else
            UNLIKELY_IF (state.fired.get().fetch_sub(detail::epoch, std::memory_order_release) == detail::successfully_finished) {
                std::atomic_thread_fence(std::memory_order_acquire);
#endif
                // nobody won and I am the last one. I have to tell the suspending runner we all failed.
                UNLIKELY_IF (!i_won && state.winner.get().load(std::memory_order_acquire) == sizeof...(Ts)) {
                    owner_raw->resume(result_type(error_tag, async_all_failed_error<flow_async_agg_err_t>::make()));
                    return;
                }
            }
        }
    };
};

template <typename ... BPs>
struct flow_when_any_fast_awaitable final :
    fast_awaitable_base<flow_when_any_fast_awaitable<BPs...>,
        typename flow_when_any_fast_state<flow_when_any_fast_awaitable<BPs...>, typename BPs::O_t...>::result_delegate,
        flow_async_agg_err_t> {
    static_assert(conjunction_v<flow_impl::is_runnable_bp<BPs>...>, "BPs should be runnable blue prints");

    constexpr static size_t N = sizeof...(BPs);
    using agg_t = flow_when_any_fast_awaitable;
    using state_t = flow_when_any_fast_state<flow_when_any_fast_awaitable<BPs...>, typename BPs::O_t...>;
    using result_delegate = typename state_t::result_delegate;
    using result_type = typename state_t::result_type;

    using async_result_type = result_type;
    using bp_pack_storage_t = flat_storage<compressed_pair<lite_ptr<BPs>, std::decay_t<typename BPs::I_t::value_type>>...>;

    bp_pack_storage_t packs;
    state_t state_;

private:
    template <size_t I>
    int launch() {
        auto& bp = get<I>(this->packs).first();
        UNLIKELY_IF (!bp) {
            return -1;
        }

#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        try {
#endif
            state_.fired.get().fetch_add(detail::epoch, std::memory_order_release);

            using receiver_t = typename state_t::template delegate<I>;
            auto runner = make_fast_runner(bp, receiver_t(this));
            runner(std::move(get<I>(this->packs).second()));
            return 0;
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        } catch (...) {
            state_.fired.get().fetch_sub(detail::epoch, std::memory_order_acq_rel);
            return -1;
        }
#endif
    }

    template <size_t... idx>
    int submit(std::index_sequence<idx...>) {
        bool keep_launching = true;
        int count = 0;
        using swallow = int[];
        (void)swallow {
                0, (keep_launching && ((count += !launch<idx>()),
                    keep_launching = (state_.winner.get().load(std::memory_order_acquire) == N)), 0)...
        };
        return count;
    }

public:
    template <typename... Args,
#if FLUEX_FOUNDRY_HAS_EXCEPTIONS
            std::enable_if_t<std::is_constructible<bp_pack_storage_t, Args&&...>::value>* = nullptr
#else
            std::enable_if_t<std::is_nothrow_constructible<bp_pack_storage_t, Args&&...>::value>* = nullptr
#endif
    >
    explicit flow_when_any_fast_awaitable(Args&&... args)
#if !FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    noexcept(std::is_nothrow_constructible<bp_pack_storage_t, Args&&...>::value)
#endif
    : packs(std::forward<Args>(args)...), state_() {}

#if !FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    bool available() const noexcept {
        return true;
    }
#endif

    int submit() noexcept {
        auto ret = this->submit(std::index_sequence_for<BPs...>{});

        UNLIKELY_IF (ret == 0) {
            state_.fired.get().fetch_or(detail::launch_failed_msk, std::memory_order_release);
            return -1;
        }

#if FLUEX_FOUNDRY_WITH_TSAN
            UNLIKELY_IF (state_.fired.get().fetch_or(detail::launch_success_msk, std::memory_order_acq_rel) == 0) {
#else
        UNLIKELY_IF (state_.fired.get().fetch_or(detail::launch_success_msk, std::memory_order_release) == 0) {
            std::atomic_thread_fence(std::memory_order_acquire);
#endif
            UNLIKELY_IF (state_.winner.get().load(std::memory_order_relaxed) == N) {
                this->resume(result_type(error_tag, async_all_failed_error<flow_async_agg_err_t>::make()));
            }
        }

        return 0;
    }

    // no cancel, do nothing
    void cancel() noexcept {}
};

template <typename... BPs>
struct flow_when_any_awaitable final :
        awaitable_base<flow_when_any_awaitable<BPs...>,
            typename flow_when_any_state<flow_when_any_awaitable<BPs...>, typename BPs::O_t...>::result_delegate,
            flow_async_agg_err_t> {
    static_assert(conjunction_v<flow_impl::is_runnable_bp<BPs>...>, "BPs should be runnable blue prints");

    constexpr static size_t N = sizeof...(BPs);
    using agg_t = flow_when_any_awaitable;
    using state_t = flow_when_any_state<flow_when_any_awaitable<BPs...>, typename BPs::O_t...>;
    using result_delegate = typename state_t::result_delegate;
    using result_type = typename state_t::result_type;

    using async_result_type = result_type;
    using bp_pack_storage_t = flat_storage<compressed_pair<lite_ptr<BPs>, std::decay_t<typename BPs::I_t::value_type>>...>;

    bp_pack_storage_t packs;
    state_t state_;

private:
    template <size_t I>
    int launch() {
        auto& bp = get<I>(this->packs).first();
        UNLIKELY_IF (!bp) {
            return -1;
        }

#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        try {
#endif
            state_.fired.get().fetch_add(detail::epoch, std::memory_order_release);

            using pack_t = flat_storage_element_t<I, bp_pack_storage_t>;
            using bp_ptr_t = typename pack_t::first_type;
            using bp_t = typename bp_ptr_t::element_type;
            using receiver_t = typename state_t::template delegate<I>;
            auto controller = &state_.controllers[I];

            using runner_t = flow_runner<bp_t, receiver_t, decltype(controller)>;
            runner_t runner(bp, controller, receiver_t(this));
            runner(std::move(get<I>(this->packs).second()));
            return 0;
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        } catch (...) {
            state_.fired.get().fetch_sub(detail::epoch, std::memory_order_acq_rel);
            return -1;
        }
#endif
    }

    template <size_t... idx>
    int submit(std::index_sequence<idx...>) {
        bool keep_launching = true;
        int count = 0;
        using swallow = int[];
        (void)swallow {
            0, (keep_launching && ((count += !launch<idx>()),
                keep_launching = (state_.winner.get().load(std::memory_order_acquire) == N)), 0)...
        };
        return count;
    }

public:
    template <typename... Args,
#if FLUEX_FOUNDRY_HAS_EXCEPTIONS
        std::enable_if_t<std::is_constructible<bp_pack_storage_t, Args&&...>::value>* = nullptr
#else
        std::enable_if_t<std::is_nothrow_constructible<bp_pack_storage_t, Args&&...>::value>* = nullptr
#endif
    >
    explicit flow_when_any_awaitable(Args&&... args)
#if !FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        noexcept(std::is_nothrow_constructible<bp_pack_storage_t, Args&&...>::value)
#endif
        : packs(std::forward<Args>(args)...), state_() {}

#if !FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    bool available() const noexcept {
        return true;
    }
#endif

    int submit() noexcept {
        auto ret = this->submit(std::index_sequence_for<BPs...>{});

        UNLIKELY_IF (ret == 0) {
            state_.fired.get().fetch_or(detail::launch_failed_msk, std::memory_order_release);
            return -1;
        }

#if FLUEX_FOUNDRY_WITH_TSAN
        UNLIKELY_IF (state_.fired.get().fetch_or(detail::launch_success_msk, std::memory_order_acq_rel) == 0) {
#else
        UNLIKELY_IF (state_.fired.get().fetch_or(detail::launch_success_msk, std::memory_order_release) == 0) {
            std::atomic_thread_fence(std::memory_order_acquire);
#endif
            UNLIKELY_IF (state_.winner.get().load(std::memory_order_relaxed) == N) {
                this->resume(result_type(error_tag, async_all_failed_error<flow_async_agg_err_t>::make()));
            }
        }

        return 0;
    }

    void cancel() noexcept {
        UNLIKELY_IF (state_.fired.get().load(std::memory_order_acquire) & detail::launch_failed_msk) {
            return;
        }

        for (size_t i = 0; i < N; ++i) {
            state_.controllers[i].cancel(true);
        }
    }
};

template <typename awaitable, typename... BPs>
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

    static_assert(is_awaitable_v<awaitable> || is_fast_awaitable_v<awaitable>,
                  "Awaitable must be an valid awaitable(see flux_foundry::awaitable_base)\n"
                  "or a valid fast_awaitable(see flux_foundry::fast_awaitable_base)");

    using node_error_t = typename awaitable::async_result_type::error_type;
    using awaitable_t = awaitable;

    flat_storage<lite_ptr<BPs>...> bps;

    explicit aggregator_awaitable_factory(lite_ptr<BPs>... bps_) noexcept
        : bps(std::move(bps_)...) {
    }

    template <size_t... I, typename... Args>
    auto create_awaitable(std::index_sequence<I...>, flat_storage<Args...>&& params) {
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        return new awaitable(make_compressed_pair(get<I>(this->bps), std::move(get<I>(params)))...);
#else
        return new (std::nothrow) awaitable(make_compressed_pair(get<I>(this->bps), std::move(get<I>(params)))...);
#endif
    }

    template <typename A = awaitable, typename... Args,
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
            auto aw = create_awaitable(std::index_sequence_for<BPs...>{}, std::move(params.value()));
            return result_t<typename awaitable::access_delegate, node_error_t>(value_tag, aw->delegate());
        } catch (...) {
            return result_t<typename awaitable::access_delegate, node_error_t>(error_tag, std::current_exception());
        }
#else
        auto aw = create_awaitable(std::index_sequence_for<BPs...>{}, std::move(params.value()));
        UNLIKELY_IF (!aw) {
            return result_t<typename awaitable::access_delegate, node_error_t>(error_tag,
                awaitable_creating_error<node_error_t>::make());
        }

        UNLIKELY_IF (!aw->available()) {
            aw->release();
            return result_t<typename awaitable::access_delegate, node_error_t>(error_tag,
                awaitable_creating_error<node_error_t>::make());
        }

        return result_t<typename awaitable::access_delegate, node_error_t>(value_tag, aw->delegate());
#endif
    }
};

} // namespace flux_foundry

#endif // FLUX_FOUNDRY_FLOW_ASYNC_AGGREGATOR_H
