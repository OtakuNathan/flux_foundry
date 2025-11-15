#ifndef __LITE_FNDS_FUTURE_TASK_H__
#define __LITE_FNDS_FUTURE_TASK_H__

#include <future>
#include "../base/traits.h"
#include "task_core.h"

namespace lite_fnds {

template <typename Callable, typename... Params>
    class future_task : task < std::decay_t<Callable>, std::decay_t<Params>...> {
    using base = task < std::decay_t<Callable>, std::decay_t<Params>...>;
    base& _as_base() noexcept {
        return static_cast<base&>(*this);
    }

    void run(std::false_type) noexcept {
        auto result = _as_base()();
        if (result.has_value()) {
            promise_.set_value(std::move(result.value()));
        } else {
            promise_.set_exception(result.error());
        }
    }

    void run(std::true_type) noexcept {
        auto result = _as_base()();
        if (result.has_value()) {
            promise_.set_value();
        } else {
            promise_.set_exception(result.error());
        }
    }

public:
    using base::base;
    using result_type = typename base::callable_result_t;
    
    future_task() = delete;
    
    future_task(const future_task&) = delete;
    future_task& operator=(const future_task&) = delete;


    future_task(future_task&& other) noexcept(conjunction_v<
        std::is_nothrow_move_constructible<base>,
        std::is_nothrow_move_constructible<std::promise<result_type>>>) : 
        base(std::move(static_cast<base&>(other))), 
        promise_(std::move(other.promise_)),
        fired_(other.fired_.load(std::memory_order_relaxed)) {
    }

    future_task& operator=(future_task&& other) noexcept(conjunction_v<
        std::is_nothrow_move_assignable<base>, 
        std::is_nothrow_move_assignable<std::promise<result_type>>
    >) {
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

    void operator()() noexcept {
        if (fired_.exchange(true, std::memory_order_relaxed)) {
            return;
        }
        this->run(std::is_void<result_type>{});
    }

private:
    std::promise<result_type> promise_;
    std::atomic<bool> fired_ { false };
};

template <typename Callable, typename... Args>
auto make_future_task(Callable&& callable, Args&&... args) 
    noexcept(std::is_nothrow_constructible<std::decay_t<Callable>, std::decay_t<Args>...>::value)
    -> future_task<std::decay_t<Callable>, std::decay_t<Args>...> {
    return future_task<std::decay_t<Callable>, std::decay_t<Args>...>(
        std::forward<Callable>(callable), std::forward<Args>(args)...);
}

}

#endif
