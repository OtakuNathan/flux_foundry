#ifndef LITE_FNDS_FUTURE_TASK_H
#define LITE_FNDS_FUTURE_TASK_H

#include <future>
#include <atomic>
#include "../base/traits.h"
#include "task_core.h"

namespace lite_fnds {

namespace future_task_detail {
    template <typename Callable, typename... Params>
    class future_task_impl : task<std::decay_t<Callable>, std::decay_t<Params>...> {
        using base = task<std::decay_t<Callable>, std::decay_t<Params>...>;

        base& _as_base() noexcept {
            return static_cast<base&>(*this);
        }

        void run(std::false_type, std::false_type) noexcept {
            auto result = _as_base()();
            if (result.has_value()) {
                promise_.set_value(std::move(result.value()));
            } 
#if LFNDS_COMPILER_HAS_EXCEPTIONS
              else {
                promise_.set_exception(result.error());
            }
#endif
        }

        void run(std::true_type, std::false_type) noexcept {
            auto result = _as_base()();
            if (result.has_value()) {
                promise_.set_value();
            }
#if LFNDS_COMPILER_HAS_EXCEPTIONS
              else {
                promise_.set_exception(result.error());
            }
#endif
        }

        void run(std::false_type, std::true_type) noexcept {
            promise_.set_value(_as_base()());
        }

    public:
        using base::base;
        using result_type = typename base::callable_result_t;

        future_task_impl() = delete;

        future_task_impl(const future_task_impl&) = delete;
        future_task_impl& operator=(const future_task_impl&) = delete;

        future_task_impl(future_task_impl&& other) noexcept(conjunction_v<
            std::is_nothrow_move_constructible<base>,
            std::is_nothrow_move_constructible<std::promise<result_type>>>)
            : base(std::move(static_cast<base&>(other)))
            , promise_(std::move(other.promise_))
            , fired_(other.fired_.load(std::memory_order_relaxed)) {
        }

        future_task_impl& operator=(future_task_impl&& other) 
            noexcept(conjunction_v<std::is_nothrow_move_assignable<base>,
                    std::is_nothrow_move_assignable<std::promise<result_type>>>) {
            if (this != &other) {
                static_cast<base&>(*this) = std::move(static_cast<base&>(other));
                promise_ = std::move(other.promise_);
                fired_.store(other.fired_.load(std::memory_order_relaxed), std::memory_order_relaxed);
            }
            return *this;
        }

        std::future<result_type> get_future() {
            return this->promise_.get_future();
        }

        // intentionally tagged as noexcept.
        void operator()() noexcept {
            if (fired_.exchange(true, std::memory_order_relaxed)) {
                return;
            }
            this->run(std::is_void<result_type> {}, is_result_t<result_type>{});
        }

    private:
        std::promise<result_type> promise_;
        std::atomic<bool> fired_ { false };
    };
}

// it's your responsibility to guarantee not to call get_future more than once.
template <typename Callable, typename... Params>
class future_task : 
    public future_task_detail::future_task_impl<Callable, Params...>,
    private ctor_delete_base<future_task<Callable, Params...>, false,
#if LFNDS_HAS_EXCEPTIONS
        true              
#else
        std::is_nothrow_move_constructible<
           future_task_detail::future_task_impl<Callable, Params...>
        >::value
#endif
    >,
    private assign_delete_base<future_task<Callable, Params...>, false,
#if LFNDS_HAS_EXCEPTIONS
        true
#else
        std::is_nothrow_move_assignable<
            future_task_detail::future_task_impl<Callable, Params...>
        >::value
#endif
     > {
    using impl = future_task_detail::future_task_impl<Callable, Params...>;
public:
    using impl::impl;
    using result_type = typename impl::result_type;
};

template <typename Callable, typename... Args>
auto make_future_task(Callable&& callable, Args&&... args) 
    noexcept(std::is_nothrow_constructible<
        future_task<std::decay_t<Callable>, std::decay_t<Args>...>,
        Callable&&, Args&&...>::value)
    -> future_task<std::decay_t<Callable>, std::decay_t<Args>...> {
    return future_task<std::decay_t<Callable>, std::decay_t<Args>...>(
        std::forward<Callable>(callable), std::forward<Args>(args)...);
}

}

#endif
