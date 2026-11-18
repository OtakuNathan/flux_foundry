// Derived from dynabridge/extensions/flux_foundry/uv_executor.h, commit
// 99d363fcff63c205d780cb7ff4ce018213c42357 (used by pal-shell-native).
// Copyright (c) 2026 Nathan Wu (OtakuNathan)
// SPDX-License-Identifier: MIT
// Integrated into FF under MIT with the repository owner's authorization.
// Modified 2026-11-13: atomic ticket admission and close/send quiescence,
// independent handle lifetime, allocation-free draining, configurable budget.
#ifndef FLUX_FOUNDRY_UV_EXECUTOR_H
#define FLUX_FOUNDRY_UV_EXECUTOR_H

#include <uv.h>
#include <cassert>
#include <atomic>
#include <cstddef>
#include <new>
#include <exception>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include "../task/task_wrapper.h"
#include "../utility/close_gate.h"

namespace flux_foundry {
// Construct on the thread which will own/run the supplied loop. The loop is
// borrowed, not a thread pool. Submission methods and close may be concurrent,
// provided the executor object itself remains alive. No concurrent uv_run or
// recursive uv_run is allowed. Run the loop through the uv_close callback.
class uv_executor {
public:
    using task_t = task_wrapper_sbo;
    static_assert(std::is_nothrow_move_constructible<task_t>::value
            && std::is_nothrow_move_assignable<task_t>::value
            && std::is_nothrow_destructible<task_t>::value,
        "uv_executor requirement: queued tasks must support nonthrowing move construction, move assignment and destruction.");
    static constexpr std::size_t max_tasks_per_round = 16;
    static constexpr std::size_t default_capacity = 1024;

    // Old libuv runtimes need an explicit fence before a coalesced send.
    // always_fence is a conservative opt-in, also useful for matched controls.
    enum class wake_fence_policy { runtime_compatible, always_fence };

    // Checked initialization reports invalid parameters / libuv errors. General
    // allocation exhaustion follows the program's std allocator policy.
    uv_executor(std::nothrow_t, uv_loop_t* loop,
                std::size_t capacity = default_capacity,
                std::size_t budget = max_tasks_per_round,
                wake_fence_policy fence = wake_fence_policy::runtime_compatible) {
        if (!loop || !capacity || !budget) { error_ = UV_EINVAL; return; }
        auto state = std::make_shared<state_t>(loop, capacity, budget, fence);
        error_ = uv_async_init(loop, &state->async, &state_t::on_async);
        if (error_) return;
        state->async.data = state.get();
        state->handle_owner = state;
        state_ = std::move(state);
    }
    explicit uv_executor(uv_loop_t* loop, std::size_t capacity = default_capacity,
                         std::size_t budget = max_tasks_per_round,
                         wake_fence_policy fence = wake_fence_policy::runtime_compatible)
        : uv_executor(std::nothrow, loop, capacity, budget, fence) {
        if (error_) {
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
            if(error_==UV_EINVAL) throw std::invalid_argument("uv_executor requires loop, capacity and budget");
            throw std::runtime_error(uv_strerror(error_));
#else
            assert(false && "uv_executor initialization failed; use checked constructor");
            std::terminate();
#endif
        }
    }
    int error() const noexcept { return error_; }
    uv_executor(const uv_executor&) = delete;
    uv_executor& operator=(const uv_executor&) = delete;
    ~uv_executor() noexcept { close(); }

    // False leaves task unconsumed: closed/full/empty/uninitialized. Submission never allocates. True
    // transfers the task exactly once, possibly running it inline on the owner.
    bool try_dispatch(task_t&& task) noexcept {
        if (!task || !state_) return false;
        // The public object must outlive concurrent producer calls. On the
        // owner-inline path a task may destroy it, but handle_owner keeps
        // state alive until the loop's close callback; recursive uv_run is
        // forbidden. The admission ticket prevents close while sending.
        auto* const state = state_.get();
        if (!state->gate.try_acquire()) return false;
        if (std::this_thread::get_id() == state->loop_thread && state->queue.empty()) {
            task(); // May close or destroy the public executor.
            state->gate.release();
            return true;
        }
        if (!state->queue.push(std::move(task))) {
            state->gate.release();
            return false;
        }
        // Hold admission THROUGH the send, even if the loop already consumed
        // this task. No sender may access async after dropping its ticket.
        state->wake();
        state->gate.release();
        return true;
    }

    // Enqueues an accepted prefix without allocation or waiting for capacity.
    // Unlike try_dispatch, this operation NEVER invokes tasks inline, even on
    // the owner. It stops at the first empty task or full queue. The returned
    // prefix has been moved; every task in the remaining suffix is untouched.
    // The array must stay alive until return. Concurrent batches may interleave
    // at task boundaries; only each producer's order is preserved.
    std::size_t try_dispatch_batch(task_t* tasks, std::size_t count) noexcept {
        if (!tasks || !count || !state_) return 0;
        auto* const state = state_.get();
        if (!state->gate.try_acquire()) return 0;
        std::size_t accepted = 0;
        while (accepted < count && tasks[accepted]
                && state->queue.push(std::move(tasks[accepted]))) {
            ++accepted;
        }
        // One admission ticket covers the whole batch, including the send.
        // Close may run after admission; it still drains every accepted task.
        if (accepted) state->wake();
        state->gate.release();
        return accepted;
    }

    // FF's void dispatch contract has no error channel. Rejection is a caller
    // contract violation; it must never silently drop an FF continuation.
    void dispatch(task_t&& task) noexcept {
        if (!try_dispatch(std::move(task))) {
            assert(false && "uv_executor dispatch rejected (closed/full/empty/uninitialized)");
            std::terminate();
        }
    }

    // Stops admission and drains accepted tasks; does not wait or stop the
    // loop, nor promise cancellation of unrelated I/O owned by that loop.
    void close() noexcept {
        const auto state = state_;
        if (!state) return;
        if (!state->gate.request_close(true)) return;
        state->wake();
        state->gate.release();
    }
    bool closed() const noexcept {
        if (!state_) return true;
        return state_->handle_closed.load(std::memory_order_acquire) != 0;
    }
    uv_loop_t* loop() const noexcept {
        if (!state_) return nullptr;
        return state_->gate.closing() ? nullptr : state_->loop;
    }

private:
    // Runtime-sized MPSC ring. Reserve bounded occupancy before assigning a
    // slot; occupancy includes unpublished reservations. A paused publisher
    // cannot be overwritten or mistaken for an empty queue. Only the loop pops.
    struct task_queue {
        struct slot {
            std::atomic<std::size_t> ready{0};
            task_t task;
        };
        const std::size_t capacity;
        std::unique_ptr<slot[]> slots;
        std::atomic<std::size_t> count{0}, tail{0};
        std::size_t head = 0;
        explicit task_queue(std::size_t n) : capacity(n), slots(new slot[n]) {}
        bool empty() const noexcept { return count.load(std::memory_order_acquire) == 0; }
        bool ready() const noexcept { return slots[head].ready.load(std::memory_order_acquire) != 0; }
        bool push(task_t&& task) noexcept {
            auto size = count.load(std::memory_order_acquire);
            do {
                if (size == capacity) return false;
            } while (!count.compare_exchange_weak(size, size + 1,
                std::memory_order_acq_rel, std::memory_order_acquire));
            auto index = tail.load(std::memory_order_acquire);
            while (!tail.compare_exchange_weak(index, index + 1 == capacity ? 0 : index + 1,
                std::memory_order_acq_rel, std::memory_order_acquire)) {}
            // Wrapping indices is safe: count reserves a distinct free slot
            // before this CAS. No monotonically growing sequence can overflow.
            // Tail RMWs MUST transfer each producer's count-acquire to its
            // successors. Our occupancy reservation may predate this slot's
            // last consumption; a later reservation, ordered before us at tail,
            // acquired that consumer's release. Relaxed tail loses that edge.
            auto& target = slots[index];
            target.task = std::move(task);
            target.ready.store(1, std::memory_order_release);
            return true;
        }
        bool pop(task_t& task) noexcept {
            auto& source = slots[head];
            if (!source.ready.load(std::memory_order_acquire)) return false;
            task = std::move(source.task);
            source.ready.store(0, std::memory_order_release);
            head = head + 1 == capacity ? 0 : head + 1;
            // Publish the freed slot before allowing another reservation.
            count.fetch_sub(1, std::memory_order_acq_rel);
            return true;
        }
    };
    struct state_t {
        uv_loop_t* const loop;
        const std::thread::id loop_thread;
        const std::size_t budget;
        const bool needs_wake_fence;
        detail::close_gate gate;
        task_queue queue;
        uv_async_t async{};
        bool handle_closing = false; // Only the loop owner calls uv_close.
        std::atomic<std::size_t> handle_closed{0};
        std::shared_ptr<state_t> handle_owner;

        state_t(uv_loop_t* target, std::size_t cap, std::size_t batch, wake_fence_policy fence)
            : loop(target), loop_thread(std::this_thread::get_id()), budget(batch),
              needs_wake_fence(fence == wake_fence_policy::always_fence || uv_version() < 0x013500),
              queue(cap) {}
        // Called with an admission ticket or from the owner callback before
        // uv_close. Slot release/acquire publishes tasks independently of uv.
        void wake() noexcept {
            // Since 1.53, uv_async_send/callback provide seq_cst ordering even
            // when sends coalesce. Select by linked runtime, not header version.
            if (needs_wake_fence) std::atomic_thread_fence(std::memory_order_seq_cst);
            if (uv_async_send(&async)) std::terminate();
        }
        static void on_async(uv_async_t* handle) noexcept {
            auto* self = static_cast<state_t*>(handle->data);
            assert(std::this_thread::get_id() == self->loop_thread);
            for (std::size_t count = 0; count < self->budget; ++count) {
                task_t task;
                if (!self->queue.pop(task)) break;
                task(); // May request close or destroy the public executor.
            }
            if (self->gate.drained() && self->queue.empty() && !self->handle_closing) {
                self->handle_closing = true;
                uv_close(reinterpret_cast<uv_handle_t*>(&self->async), &state_t::on_closed);
            } else if (self->queue.ready() || self->gate.closing()) {
                // A send can trigger this callback BEFORE its ticket is
                // released. Rearm on the owner; never wake after last release
                // on a producer, which could race handle closure.
                self->wake();
            }
        }
        static void on_closed(uv_handle_t* handle) noexcept {
            auto* self = static_cast<state_t*>(handle->data);
            auto owner = std::move(self->handle_owner);
            self->handle_closed.store(1, std::memory_order_release);
        }
    };
    std::shared_ptr<state_t> state_;
    int error_ = 0;
};
}
#endif
