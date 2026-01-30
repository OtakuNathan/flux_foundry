//
// Created by Nathan on 1/15/2026.
//

#ifndef LITE_FNDS_SIMPLE_EXECUTOR_H
#define LITE_FNDS_SIMPLE_EXECUTOR_H

#include <cassert>
#include <atomic>
#include "../utility/back_off.h"
#include "../utility/concurrent_queues.h"
#include "../task/task_wrapper.h"

namespace lite_fnds {
    template <size_t capacity>
    class simple_executor {
        enum class control_flag : uint8_t {
            idle     = 0,
            running  = 1,
            shutdown = 2,
        };

        std::atomic<uint8_t> flag_{static_cast<uint8_t>(control_flag::idle)};
        mpsc_queue<task_wrapper_sbo, capacity> q;

        static simple_executor*& current() noexcept {
            thread_local simple_executor* executor = nullptr;
            return executor;
        }
    public:
        simple_executor() noexcept = default;

        void dispatch(task_wrapper_sbo&& sbo) noexcept {
            if (flag_.load(std::memory_order_relaxed) & static_cast<uint8_t>(control_flag::shutdown)) {
                assert(false && "executor is shutdown.");
                std::abort();
            }

            backoff_strategy<> backoff;
            for (; !q.try_emplace(std::move(sbo)); backoff.yield()) {
                if (current() == this) {
                    sbo();
                    break;
                }
            }
        }

        // Contract:
        // - `run()` must be called by at most one thread at a time for this executor instance.
        // - `run()` must NOT be re-entered or nested on the same thread (e.g., calling `run()` from a task).
        void run() noexcept {
            auto curr = static_cast<uint8_t>(control_flag::idle);
            if (!flag_.compare_exchange_strong(curr, static_cast<uint8_t>(control_flag::running),
                std::memory_order_relaxed, std::memory_order_relaxed)) {
                return;
            }

            assert(current() == nullptr && "simple_executor::run() must not be nested/re-entered on the same thread");
            current() = this;
            for (backoff_strategy<> backoff; !(flag_.load(std::memory_order_relaxed) & static_cast<uint8_t>(control_flag::shutdown)); ) {
                auto p = q.try_pop();
                if (p.has_value()) {
                    p.get()();
                    backoff.reset();
                } else {
                    backoff.yield();
                }
            }

            // clear pending queue.
            while (auto p = q.try_pop()) {
                p.get()();
            }

            current() = nullptr;
            flag_.store(static_cast<uint8_t>(control_flag::idle), std::memory_order_relaxed);
        }

        void shutdown() noexcept {
            flag_.fetch_or(static_cast<uint8_t>(control_flag::shutdown), std::memory_order_relaxed);
        }
    };
}

#endif // LITE_FNDS_SIMPLE_EXECUTOR_H
