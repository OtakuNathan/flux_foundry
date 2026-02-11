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
        enum class control_flag : uint8_t {
            idle     = 0,
            running  = 1,
            shutdown = 2,
        };
        
        padded_t<std::atomic<size_t>> pending_{0};
        padded_t<std::atomic<uint8_t>> state_{static_cast<uint8_t>(control_flag::idle)};
        mpsc_queue<task_wrapper_sbo, capacity> q;

        static simple_executor*& current() noexcept {
            thread_local simple_executor* executor = nullptr;
            return executor;
        }
    public:
        simple_executor() noexcept = default;

        // Thread-safe for producer side.
        // Tasks that "buy a ticket" (pending++) are guaranteed to be either:
        // - enqueued and later consumed by run(), or
        // - executed inline by the consumer thread when queue is full.
        void dispatch(task_wrapper_sbo&& sbo) noexcept {
            auto& pending = pending_.get();
            auto& state = state_.get();
            pending.fetch_add(1, std::memory_order_relaxed);
            if (state.load(std::memory_order_relaxed) & static_cast<uint8_t>(control_flag::shutdown)) {
                assert(false && "executor is shutdown.");
                pending.fetch_sub(1, std::memory_order_relaxed);
                std::abort();
            }

            backoff_strategy<> backoff;
            for (; !q.try_emplace(std::move(sbo)); backoff.yield()) {
                if (current() == this) {
                    pending.fetch_sub(1, std::memory_order_relaxed);
                    sbo();
                    break;
                }
            }
        }

        // Contract:
        // - `run()` must be called by at most one thread at a time for this executor instance.
        // - `run()` must NOT be re-entered or nested on the same thread (e.g., calling `run()` from a task).
        // - returns only after shutdown is observed and all admitted tasks are drained.
        void run() noexcept {
            auto& pending = pending_.get();
            auto& state = state_.get();

            auto curr = static_cast<uint8_t>(control_flag::idle);
            if (!state.compare_exchange_strong(curr, static_cast<uint8_t>(control_flag::running),
                std::memory_order_relaxed, std::memory_order_relaxed)) {
                return;
            }

            assert(current() == nullptr && "simple_executor::run() must not be nested/re-entered on the same thread");
            current() = this;
            for (backoff_strategy<> backoff; !(state.load(std::memory_order_relaxed) & static_cast<uint8_t>(control_flag::shutdown)); ) {
                auto p = q.try_pop();
                if (p) {
                    p.get()();
                    pending.fetch_sub(1, std::memory_order_relaxed);
                    backoff.reset();
                } else {
                    backoff.yield();
                }
            }

            // clear pending queue.
            for (backoff_strategy<> backoff; pending.load(std::memory_order_relaxed); backoff.yield()) {
                auto p = q.try_pop();
                if (p) {
                    p.get()();
                    pending.fetch_sub(1, std::memory_order_relaxed);
                    backoff.reset();
                }
            }

            current() = nullptr;
        }
        
        // Producer/control thread API.
        // Returns true when shutdown transition is visible/successful.
        bool try_shutdown() noexcept {
            auto& state = state_.get();
            uint8_t exp = static_cast<uint8_t>(control_flag::running);
            LIKELY_IF (state.compare_exchange_weak(exp,
                    static_cast<uint8_t>(control_flag::shutdown),
                    std::memory_order_relaxed, std::memory_order_relaxed)) {
                return true;
            }
            return exp == static_cast<uint8_t>(control_flag::shutdown);
        }
    };
}

#endif // FLUX_FOUNDRY_SIMPLE_EXECUTOR_H
