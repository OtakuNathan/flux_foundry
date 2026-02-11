
//
// Created by Nathan on 1/27/2026.
//

#ifndef FLUX_FOUNDRY_FLOW_AWAITABLE_H
#define FLUX_FOUNDRY_FLOW_AWAITABLE_H

#include <new>
#include "../memory/lite_ptr.h"
#include "../memory/result_t.h"
#include "../utility/callable_wrapper.h"
#include "flow_def.h"

namespace flux_foundry {
    // Contract: Awaitables in flux_foundry MUST NOT start any side effects before submit_async() is called.
    template <typename derived, typename T, typename E>
    struct awaitable_base {
    private:

        // State transitions:
        // idle -> waiting (via submit_async, atomic CAS)
        // waiting -> done (via resume, atomic CAS)
        // Only one thread can successfully transition from idle->waiting
        // Only one thread (either async completion or cancel) can transition waiting->done.
        enum wait_state {
            idle, // No operation in progress
            waiting, // Waiting for async operation
            done, // Operation done, callback not run yet
        };

        using next_step_t = callable_wrapper<void(result_t<T, E>&&)>;
        std::atomic<wait_state> status;
        std::atomic<size_t> refcount;
        next_step_t next_step;

        static void notify_cancel_handler_dropped(void* self_) noexcept {
            auto self = static_cast<awaitable_base*>(self_);
            self->release();
        }

        void do_resume(result_t<T, E>&& result) noexcept {
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            try {
#endif
                this->next_step(std::move(result));
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            } catch (...) {
                this->next_step(result_t<T, E>(error_tag, std::current_exception()));
            }
#endif
            release();
        }

        static void cancel(void* self_, cancel_kind kind) noexcept {
            auto self = static_cast<awaitable_base*>(self_);
            auto expected = waiting;
            if (!self->status.compare_exchange_strong(expected, done,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
                return;
            }

            static_cast<derived*>(self)->cancel();
            result_t<T, E> cancel_result(error_tag, flux_foundry::cancel_error<E>::make(kind));
            self->do_resume(std::move(cancel_result));
        }
    public:
        struct access_delegate {
            awaitable_base* self;

            void emplace_nextstep(next_step_t&& next) noexcept {
                self->next_step = std::move(next);
            }

            int submit_async() noexcept {
                // can only submit once
                auto expected = idle;
                if (!self->status.compare_exchange_strong(expected, waiting,
                    std::memory_order_release, std::memory_order_relaxed)) {
                    return -1;
                }

                auto code = static_cast<derived*>(self)->submit();
                if (code != 0) {
                    self->status.store(idle, std::memory_order_release);
                    return code;
                }

                return 0;
            }

            void provide_cancel_handler(detail::flow_async_cancel_handler_t* cancel_handler,
                detail::flow_async_notify_handler_dropped_t* drop, 
                detail::flow_async_cancel_param_t* param) noexcept {
                *cancel_handler = cancel;
                *drop = notify_cancel_handler_dropped;
                *param = self;
                self->retain();
            }

            void release() noexcept {
                self->release();
            }
        };

        // NEVER EVER CALL THIS BY HAND
        access_delegate delegate() noexcept {
            return access_delegate{ this };
        }

        // [Usage Contract]
        // Call this in your submit() implementation IF AND ONLY IF the async backend
        // holds a reference to this object (i.e., it will trigger a callback later).
        // This creates a "Backend Reference" preventing the object from being destroyed
        // prematurely by cancellation.
        void retain() noexcept {
            refcount.fetch_add(1, std::memory_order_relaxed);
        }

        // [Usage Contract]
        // Call this in your call back implementation right after the calling of resume()
        // IF AND ONLY IF the async backend holds a reference to this object (i.e., it will trigger a callback later).
        // This releases a "Backend Reference" preventing the object from being leaked
        void release() noexcept {
            UNLIKELY_IF(refcount.fetch_sub(1, std::memory_order_release) == 1) {
                std::atomic_thread_fence(std::memory_order_acquire);
                delete static_cast<derived*>(this);
            }
        }

        // THIS IS THE ONLY FUNCTION YOU COULD SAFELY CALL BY HAND,
        // awaitable implementation should call resume(result_t<T, E>) after it finished the async operation,
        // CRITICAL: CALLING RESUME MUST BE THE LAST THING YOU DO WITH (this) OF A AWAITABLE.
        void resume(result_t<T, E>&& io_result) noexcept {
            auto expected = waiting;
            if (!status.compare_exchange_strong(expected, done,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
                return;
            }
            do_resume(std::move(io_result));
        }

        awaitable_base() noexcept
            : status{ idle }, refcount(1) {
        }
    };

    template <typename T, typename = void>
    struct is_awaitable : std::false_type {};

    template <typename T>
    struct is_awaitable<T, void_t<typename T::async_result_type>>
        : std::is_base_of<awaitable_base<T,
        typename T::async_result_type::value_type,
        typename T::async_result_type::error_type>, T> {
    };

    template <typename T>
    constexpr bool is_awaitable_v = is_awaitable<T>::value;

    template <typename awaitable>
    struct awaitable_factory {
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
        constexpr static auto detect_cancel(int) ->
            std::integral_constant<bool, noexcept(std::declval<U&>().cancel())>;

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

        template <typename A = awaitable, typename ... Args,
#if FLUEX_FOUNDRY_HAS_EXCEPTIONS
            std::enable_if_t<std::is_constructible<awaitable, Args&&...>::value>* = nullptr
#else
            std::enable_if_t<std::is_nothrow_constructible<awaitable, Args&&...>::value>* = nullptr
#endif
        >
        result_t<typename awaitable::access_delegate, node_error_t> operator()(Args&& ... param) noexcept {
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            try {
                auto aw = new awaitable(std::forward<Args>(param)...);
                return result_t<typename awaitable::access_delegate, node_error_t>(value_tag, aw->delegate());
            } catch (...) {
                return result_t<typename awaitable::access_delegate, node_error_t>(error_tag, std::current_exception());
            }
#else
            auto aw = new (std::nothrow) awaitable(std::forward<Args>(param)...);
            UNLIKELY_IF (!aw) {
                return result_t<typename awaitable::access_delegate, node_error_t>(error_tag, awaitable_creating_error<node_error_t>::make());
            }

            UNLIKELY_IF(!aw->available()) {
                aw->release();
                return result_t<typename awaitable::access_delegate, node_error_t>(error_tag, awaitable_creating_error<node_error_t>::make());
            }

            return result_t<typename awaitable::access_delegate, node_error_t>(value_tag, aw->delegate());
#endif
        }
    };

    template <typename T>
    struct is_awaitable_factory : std::false_type {};

    template <typename awaitable>
    struct is_awaitable_factory<awaitable_factory<awaitable>> : std::true_type {};

    template <typename T>
    constexpr bool is_awaitable_factory_v = is_awaitable_factory<T>::value;

}

#endif // FLUX_FOUNDRY_FLOW_AWAITABLE_H
