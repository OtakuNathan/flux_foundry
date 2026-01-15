//
// Created by Nathan on 1/15/2026.
//

#ifndef LITE_FNDS_SIMPLE_EXECUTOR_H
#define LITE_FNDS_SIMPLE_EXECUTOR_H

#include <atomic>
#include "../utility/yield.h"
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

        static bool& is_worker_thread() noexcept {
            thread_local bool is_worker_thread = false;
            return is_worker_thread;
        }
    public:

        void dispatch(task_wrapper_sbo&& sbo) noexcept {
            if (flag_.load(std::memory_order_relaxed) & static_cast<uint8_t>(control_flag::shutdown)) {
                assert(false && "executor is shutdown.");
                std::abort();
            }

            for (; !q.try_emplace(std::move(sbo)); yield()) {
                if (is_worker_thread()) {
                    sbo();
                    break;
                }
            }
        }

        void run() noexcept {
            auto curr = static_cast<uint8_t>(control_flag::idle);
            if (!flag_.compare_exchange_strong(curr, static_cast<uint8_t>(control_flag::running),
                std::memory_order_relaxed, std::memory_order_relaxed)) {
                return;
            }

            is_worker_thread() = true;
            for (; !(flag_.load(std::memory_order_relaxed) & static_cast<uint8_t>(control_flag::shutdown)); yield()) {
                auto p = q.try_pop();
                if (p.has_value()) {
                    p.get()();
                }
            }

            // clear pending queue.
            while (auto p = q.try_pop()) {
                if (p.has_value()) {
                    p.get()();
                }
            }

            is_worker_thread() = false;
        }

        void shutdown() noexcept {
            flag_.fetch_or(static_cast<uint8_t>(control_flag::shutdown), std::memory_order_relaxed);
        }

    };
}

#endif //SIMPLE_EXECUTOR_H
