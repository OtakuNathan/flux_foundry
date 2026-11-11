//
// Created by Nathan on 12/2/2025.
//

#ifndef FLUX_FOUNDRY_GSOURCE_EXECUTOR_H
#define FLUX_FOUNDRY_GSOURCE_EXECUTOR_H

#include <sstream>
#include <stdexcept>
#include <cerrno>
#include <cassert>
#include <exception>

#include <unistd.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <glib.h>

#include "../utility/concurrent_queues.h"
#include "../task/task_wrapper.h"

namespace flux_foundry {
    template <size_t capacity_>
    struct gsource_executor {
        using task_wrapper_t = task_wrapper_sbo;
        using queue_type = mpsc_queue<task_wrapper_t, capacity_>;

        constexpr static size_t capacity = capacity_;
        constexpr static size_t sbo_size = task_wrapper_t::sbo_size;
        constexpr static size_t align = task_wrapper_t::align;
        static constexpr size_t max_task_per_round = 10ull;

        struct gsource_executor_ctx : GSource {
            static gint g_source_task_proc(void *data) noexcept {
                // The GSource owns this state and GLib retains the source
                // through dispatch, including nested main-context iterations.
                auto self = static_cast<gsource_executor_ctx*>(data);

                for (uint64_t budget = 0;;) {
                    ssize_t r = ::read(self->m_efd, &budget, sizeof(uint64_t));
                    if (r == sizeof(uint64_t)) break;
                    if (r < 0 && errno == EINTR) continue;
                    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                    if (r <= 0) break;
                }

                bool queue_became_empty = false;
                for (size_t c = 0; c < gsource_executor::max_task_per_round && self->executor_; ++c) {
                    auto tsk = self->executor_->q_.try_pop();
                    if (!tsk) {
                        queue_became_empty = true;
                        break;
                    }

                    tsk.get()();
                }

                if (!self->executor_) return G_SOURCE_REMOVE;
                if (!queue_became_empty) {
                    if (self->schedule_wake_up(1)) std::terminate();
                }

                return TRUE;
            }

            GPollFD fd;
            gsource_executor* executor_;
            int m_efd;

            static gboolean prepare(GSource*, gint* timeout) noexcept {
                *timeout = -1;
                return FALSE;
            }

            static gboolean check(GSource* source) noexcept {
                auto* self = static_cast<gsource_executor_ctx*>(source);
                return (self->fd.revents & self->fd.events) != 0;
            }

            static gboolean dispatch(GSource*, GSourceFunc callback, gpointer user_data) noexcept {
                return callback(user_data);
            }

            static void finalize(GSource* source) noexcept {
                auto* self = static_cast<gsource_executor_ctx*>(source);
                ::close(self->m_efd);
            }

            static gsource_executor_ctx* create(gsource_executor& queue) {
                const int descriptor = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
                if (descriptor < 0) {
                    std::stringstream ss;
                    ss << "failed to create eventfd, errno: " << errno;
                    throw std::runtime_error(ss.str());
                }

                // Store callback state in the source's existing allocation.
                // The function table must also outlive the public executor.
                static GSourceFuncs functions{prepare, check, dispatch, finalize, nullptr, nullptr};
                auto* self = static_cast<gsource_executor_ctx*>(
                    g_source_new(&functions, sizeof(gsource_executor_ctx)));
                self->executor_ = &queue;
                self->m_efd = descriptor;
                self->fd.fd = descriptor;
                self->fd.events = G_IO_IN | G_IO_HUP | G_IO_ERR;
                self->fd.revents = 0;
                g_source_add_poll(self, &self->fd);
                g_source_set_priority(self, G_PRIORITY_DEFAULT);
                g_source_set_callback(self, g_source_task_proc, self, nullptr);
                g_source_set_can_recurse(self, TRUE);
                return self;
            }

            void detach() noexcept {
                executor_ = nullptr;
                g_source_destroy(this);
                // This may finalize/free the source. Active dispatch retains
                // it until the callback has returned; do not access it again.
                g_source_unref(this);
            }

            int schedule_wake_up(uint64_t n = 1) const noexcept {
                uint64_t inc = n;
                for (;;) {
                    ssize_t wrote = ::write(m_efd, &inc, sizeof(inc));
                    if (wrote == sizeof(inc)) return 0;
                    if (wrote < 0 && errno == EINTR) continue;
                    // If NONBLOCK and counter is "full", EAGAIN: treat as already-armed.
                    if (wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
                    return errno; // other errors propagate
                }
            }
        };

        gsource_executor() : ctx_(gsource_executor_ctx::create(*this)) {}

        gsource_executor(const gsource_executor&) = delete;
        gsource_executor(gsource_executor&&) noexcept = delete;
        gsource_executor& operator=(const gsource_executor&) = delete;
        gsource_executor& operator=(gsource_executor&&) noexcept = delete;
        // Stop producers before destruction. Destroy on the context owner, or
        // after its dispatch has stopped; destruction inside a task is allowed.
        // Pending queued tasks are discarded, rather than drained.
        ~gsource_executor() noexcept { ctx_->detach(); }

        int register_to(GMainContext *context) noexcept {
            if (!context) {
                return EINVAL;
            }
            if (g_source_get_context(ctx_)) return EALREADY;
            return g_source_attach(ctx_, context) ? 0 : EINVAL;
        }

        void dispatch(task_wrapper_sbo&& task) noexcept {
            assert(task && "attempting to dispatch an empty task into the executor.");
            if (!task) {
                return;
            }
            // Register before concurrent dispatch; registration/destruction must
            // not race with producers. The context owner cannot wait for space:
            // it is also the thread responsible for draining this queue.
            auto* context = g_source_get_context(ctx_);
            for (backoff_strategy<> backoff; !q_.try_emplace(std::move(task)); backoff.yield()) {
                if (context && g_main_context_is_owner(context)) {
                    // Like simple_executor, nested full-queue dispatch runs
                    // inline and may recurse; FIFO ordering is not guaranteed.
                    task();
                    return;
                }
            }
            if (ctx_->schedule_wake_up(1)) std::terminate();
        }
    private:
        // Owns one GSource reference; GLib owns its callback state and allocation.
        gsource_executor_ctx* ctx_;
        queue_type q_;
    };
}

#endif //GLIB_WAKEUP_EXECUTOR_H
