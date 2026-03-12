//
// Created by Nathan on 1/15/2026.
//

#ifndef FLUX_FOUNDRY_SIMPLE_EXECUTOR_H
#define FLUX_FOUNDRY_SIMPLE_EXECUTOR_H

#include <cassert>
#include <atomic>
#include "../utility/back_off.h"
#include "../utility/concurrent_queues.h"
#include "../task/task_wrapper.h"

namespace flux_foundry {
    template <size_t capacity>
    class simple_executor {
        // Execution model:
        // - many producer threads may call dispatch()
        // - exactly one consumer thread may call run()
        // Lifecycle model:
        // - dispatch() before run() is allowed
        // - dispatch() after shutdown is invalid usage (assert + abort)
        // - try_shutdown() requests stop, run() drains all admitted tickets before returning
        static constexpr size_t running_flag = size_t{1} << 0;
        static constexpr size_t shutdown_flag = size_t{1} << 1;
        static constexpr size_t pending_shift = 2;
        static constexpr size_t pending_unit = size_t{1} << pending_shift;

        padded_t<std::atomic<size_t>> ctrl_{0};
        mpsc_queue<task_wrapper_sbo, capacity> q;

        static simple_executor*& current() noexcept {
            thread_local simple_executor* executor = nullptr;
            return executor;
        }

        static bool is_running(size_t ctrl) noexcept {
            return (ctrl & running_flag) != 0;
        }

        static bool is_shutdown(size_t ctrl) noexcept {
            return (ctrl & shutdown_flag) != 0;
        }

        static size_t pending_count(size_t ctrl) noexcept {
            return ctrl >> pending_shift;
        }
    public:
        simple_executor() noexcept = default;

        // Thread-safe for producer side.
        // Tasks that "buy a ticket" (pending++) are guaranteed to be either:
        // - enqueued and later consumed by run(), or
        // - executed inline by the consumer thread when queue is full.
        void dispatch(task_wrapper_sbo&& sbo) noexcept {
            auto& ctrl = ctrl_.get();
            for (backoff_strategy<> gate_backoff;; gate_backoff.yield()) {
                auto state = ctrl.load(std::memory_order_acquire);
                if (is_shutdown(state)) {
                    assert(false && "executor is shutdown.");
                    std::abort();
                }

                if (ctrl.compare_exchange_weak(state, state + pending_unit,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                    break;
                }
            }

            backoff_strategy<> backoff;
            for (; !q.try_emplace(std::move(sbo)); backoff.yield()) {
                if (current() == this) {
                    sbo();
                    ctrl.fetch_sub(pending_unit, std::memory_order_acq_rel);
                    break;
                }

                auto state = ctrl.load(std::memory_order_acquire);
                if (is_shutdown(state) && !is_running(state)) {
                    ctrl.fetch_sub(pending_unit, std::memory_order_acq_rel);
                    assert(false && "executor is shutdown.");
                    std::abort();
                }
            }
        }

        // Contract:
        // - `run()` must be called by at most one thread at a time for this executor instance.
        // - `run()` must NOT be re-entered or nested on the same thread (e.g., calling `run()` from a task).
        // - returns only after shutdown is observed and all admitted tasks are drained.
        void run() noexcept {
            auto& ctrl = ctrl_.get();

            for (backoff_strategy<> gate_backoff;; gate_backoff.yield()) {
                auto state = ctrl.load(std::memory_order_acquire);
                if (is_running(state)) {
                    return;
                }

                if (ctrl.compare_exchange_weak(state, state | running_flag,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                    break;
                }
            }

            assert(current() == nullptr && "simple_executor::run() must not be nested/re-entered on the same thread");
            current() = this;
            for (backoff_strategy<> backoff;; backoff.yield()) {
                auto p = q.try_pop();
                if (p) {
                    p.get()();
                    auto state = ctrl.fetch_sub(pending_unit, std::memory_order_acq_rel);
                    backoff.reset();
                    if (is_shutdown(state) && pending_count(state) == 1) {
                        break;
                    }
                    continue;
                }

                auto state = ctrl.load(std::memory_order_acquire);
                if (is_shutdown(state) && pending_count(state) == 0) {
                    break;
                }
            }

            current() = nullptr;
            ctrl.fetch_and(~running_flag, std::memory_order_release);
        }
        
        // Producer/control thread API.
        // Returns true when shutdown transition is visible/successful.
        bool try_shutdown() noexcept {
            auto& ctrl = ctrl_.get();
            for (backoff_strategy<> backoff;; backoff.yield()) {
                auto state = ctrl.load(std::memory_order_acquire);
                if (is_shutdown(state)) {
                    return true;
                }

                if (ctrl.compare_exchange_weak(state, state | shutdown_flag,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                    return true;
                }
            }
        }
    };
}

#endif // FLUX_FOUNDRY_SIMPLE_EXECUTOR_H
