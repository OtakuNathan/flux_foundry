// Derived from dynabridge/extensions/flux_foundry/uv_executor.h, commit
// 99d363fcff63c205d780cb7ff4ce018213c42357 (used by pal-shell-native).
// Copyright (c) 2026 Nathan Wu (OtakuNathan)
// SPDX-License-Identifier: MIT
// Integrated into FF under MIT with the repository owner's authorization.
// Modified 2026-11-04: FF namespace, bounded admission, synchronized close/send,
// independent handle lifetime, allocation-free draining, configurable budget.
#ifndef FLUX_FOUNDRY_UV_EXECUTOR_H
#define FLUX_FOUNDRY_UV_EXECUTOR_H

#include <uv.h>
#include <cassert>
#include <cstddef>
#include <vector>
#include <new>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include "../task/task_wrapper.h"

namespace flux_foundry {
// Construct on the thread which will own/run the supplied loop. The loop is
// borrowed, not a thread pool. dispatch/try_dispatch/close may be concurrent,
// provided the executor object itself remains alive. No concurrent uv_run or
// recursive uv_run is allowed. Run the loop through the uv_close callback.
class uv_executor {
public:
    using task_t = task_wrapper_sbo;
    static constexpr std::size_t max_tasks_per_round = 16;
    static constexpr std::size_t default_capacity = 1024;

    // Checked initialization reports invalid parameters / libuv errors. General
    // allocation exhaustion follows the program's std allocator policy.
    uv_executor(std::nothrow_t, uv_loop_t* loop,
                std::size_t capacity = default_capacity,
                std::size_t budget = max_tasks_per_round) {
        if (!loop || !capacity || !budget) { error_ = UV_EINVAL; return; }
        auto state = std::make_shared<state_t>(loop, capacity, budget);
        error_ = uv_async_init(loop, &state->async, &state_t::on_async);
        if (error_) return;
        state->async.data = state.get();
        state->handle_owner = state;
        state_ = std::move(state);
    }
    explicit uv_executor(uv_loop_t* loop, std::size_t capacity = default_capacity,
                         std::size_t budget = max_tasks_per_round)
        : uv_executor(std::nothrow, loop, capacity, budget) {
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
        const auto state = state_; // state_ is immutable after construction.
        bool inline_task = false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->closing) return false;
            if (std::this_thread::get_id() == state->loop_thread && state->count == 0) {
                inline_task = true;
            } else {
                if (state->count == state->capacity) return false;
                state->tasks[(state->head + state->count) % state->capacity] = std::move(task);
                ++state->count;
                state->wake(); // Serialize the last send with closing the handle.
            }
        }
        // Never call user code with the queue mutex held. Inline nesting may
        // recurse; use try_dispatch at ingress if bounded rejection is needed.
        if (inline_task) task();
        return true;
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
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->closing) return;
        state->closing = true;
        state->wake();
    }
    bool closed() const noexcept {
        if (!state_) return true;
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->handle_closed;
    }
    uv_loop_t* loop() const noexcept {
        if (!state_) return nullptr;
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->closing ? nullptr : state_->loop;
    }

private:
    struct state_t {
        uv_loop_t* const loop;
        const std::thread::id loop_thread;
        const std::size_t capacity, budget;
        std::mutex mutex;
        std::vector<task_t> tasks; // Allocated once before uv_async_init.
        std::size_t head = 0, count = 0;
        uv_async_t async{};
        bool closing = false, handle_closing = false, handle_closed = false;
        std::shared_ptr<state_t> handle_owner;

        state_t(uv_loop_t* target, std::size_t cap, std::size_t batch)
            : loop(target), loop_thread(std::this_thread::get_id()), capacity(cap), budget(batch), tasks(cap) {}
        // Called under mutex before handle_closing. Queue publication uses
        // mutex synchronization, independent of libuv notification coalescing.
        void wake() noexcept {
            if (uv_async_send(&async)) std::terminate();
        }
        static void on_async(uv_async_t* handle) noexcept {
            auto* self = static_cast<state_t*>(handle->data);
            assert(std::this_thread::get_id() == self->loop_thread);
            for (std::size_t count = 0; count < self->budget; ++count) {
                task_t task;
                {
                    std::lock_guard<std::mutex> lock(self->mutex);
                    if (!self->count) break;
                    task = std::move(self->tasks[self->head]);
                    self->head = (self->head + 1) % self->capacity;
                    --self->count;
                }
                task(); // May request close or destroy the public executor.
            }
            bool close_handle = false;
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                if (self->count) self->wake(); // Notifications coalesce.
                else if (self->closing && !self->handle_closing) {
                    self->handle_closing = true;
                    close_handle = true;
                }
            }
            if (close_handle)
                uv_close(reinterpret_cast<uv_handle_t*>(&self->async), &state_t::on_closed);
        }
        static void on_closed(uv_handle_t* handle) noexcept {
            auto* self = static_cast<state_t*>(handle->data);
            auto owner = std::move(self->handle_owner);
            std::lock_guard<std::mutex> lock(self->mutex);
            self->handle_closed = true;
            // lock dies before owner; no access to self after releasing owner.
        }
    };
    std::shared_ptr<state_t> state_;
    int error_ = 0;
};
}
#endif
