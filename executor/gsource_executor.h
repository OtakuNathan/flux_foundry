//
// Created by wufen on 12/2/2025.
//

#ifndef LITE_FNDS_GSOURCE_EXECUTOR_H
#define LITE_FNDS_GSOURCE_EXECUTOR_H

#include <memory>
#include <sstream>
#include <stdexcept>
#include <cerrno>
#include <cassert>

#include <unistd.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <glib-2.0/glib.h>

#include "../utility/concurrent_queues.h"
#include "../task/task_wrapper.h"

namespace lite_fnds {
    template <size_t capacity_>
    struct gsource_executor {
        using task_wrapper_t = task_wrapper_sbo;
        using queue_type = mpsc_queue<task_wrapper_t, capacity_>;

        constexpr static size_t capacity = capacity_;
        constexpr static size_t sbo_size = task_wrapper_t::sbo_size;
        constexpr static size_t align = task_wrapper_t::align;
        static constexpr size_t max_task_per_round = 10ull;

        struct gsource_executor_ctx {
            static gint g_source_task_proc(void *data) noexcept {
                auto self = static_cast<gsource_executor_ctx*>(data);

                for (uint64_t budget = 0;;) {
                    ssize_t r = ::read(self->m_efd, &budget, sizeof(uint64_t));
                    if (r == sizeof(uint64_t)) break;
                    if (r < 0 && errno == EINTR) continue;
                    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                    if (r <= 0) break;
                }

                bool queue_became_empty = false;
                for (int c = 0; c < gsource_executor::max_task_per_round; ++c) {
                    auto tsk = self->executor_ref_.q_.try_pop();
                    if (!tsk) {
                        queue_became_empty = true;
                        break;
                    }

                    tsk.get()();
                }

                if (!queue_became_empty) {
                    (void)self->schedule_wake_up(1);
                }

                return TRUE;
            }

            struct executor_src : GSource {
                GPollFD fd;

                static gboolean prepare(GSource *source, gint *timeout) {
                    (void)source;
                    *timeout = -1;
                    return false;
                }

                static gboolean check(GSource *source) {
                    auto src_task = static_cast<executor_src *>(source);
                    return (src_task->fd.revents & src_task->fd.events) != 0;
                }

                static gboolean dispatch(GSource *source, GSourceFunc callback, gpointer user_data) {
                    (void) source;
                    return callback(user_data);
                }
            };

            gsource_executor &executor_ref_;
            GSource *src_;
            GSourceFuncs src_fns;
            int m_efd;

            explicit gsource_executor_ctx(gsource_executor &queue) :
                executor_ref_{queue}, src_{nullptr},
                src_fns{ executor_src::prepare, executor_src::check,
                         executor_src::dispatch, nullptr }, m_efd{0} {
                m_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
                if (m_efd < 0) {
                    std::stringstream ss;
                    ss << "failed to create eventfd, errno: " << errno;
                    throw std::runtime_error(ss.str());
                }

                src_ = g_source_new(&src_fns, sizeof(executor_src));
                auto source_tsk = reinterpret_cast<executor_src *>(src_);

                source_tsk->fd.fd = m_efd;
                source_tsk->fd.events = G_IO_IN | G_IO_HUP | G_IO_ERR;
                source_tsk->fd.revents = 0;

                g_source_add_poll(src_, &source_tsk->fd);
                g_source_set_priority(src_, G_PRIORITY_DEFAULT);
                g_source_set_callback(src_, (GSourceFunc)g_source_task_proc, this, nullptr);
                g_source_set_can_recurse(src_, TRUE);
            }

            ~gsource_executor_ctx() noexcept {
                if (src_) {
                    auto source_tsk = reinterpret_cast<executor_src *>(src_);
                    g_source_remove_poll(src_, &source_tsk->fd);
                    g_source_destroy(reinterpret_cast<GSource *>(src_));
                    g_source_unref(reinterpret_cast<GSource *>(src_));
                }

                if (m_efd >= 0) {
                    ::close(m_efd);
                }
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

        gsource_executor() : ctx_(*this) {}

        gsource_executor(const gsource_executor&) = delete;
        gsource_executor(gsource_executor&&) noexcept = delete;
        gsource_executor& operator=(const gsource_executor&) = delete;
        gsource_executor& operator=(gsource_executor&&) noexcept = delete;
        ~gsource_executor() noexcept = default;

        int register_to(GMainContext *context) noexcept {
            if (!context) {
                return EINVAL;
            }
            g_source_attach(ctx_.src_, context);
            return 0;
        }

        void dispatch(task_wrapper_sbo&& task) noexcept {
            assert(task && "attempting to dispatch an empty task into the executor.");
            if (!task) {
                return;
            }
            q_.wait_and_emplace(std::move(task));
            ctx_.schedule_wake_up(1);
        }
    private:
        gsource_executor_ctx ctx_;
        queue_type q_;
    };
}

#endif //GLIB_WAKEUP_EXECUTOR_H
