#ifndef FLUX_FOUNDRY_IO_URING_AWAITABLE_H
#define FLUX_FOUNDRY_IO_URING_AWAITABLE_H

#include <liburing.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>
#include "../flow/flow_awaitable.h"
#include "../flow/flow_node.h"
#include "detail/io_error.h"
#include "../utility/close_gate.h"

namespace flux_foundry {
namespace extension {

namespace detail {
template <typename Operation, typename = void>
struct uring_prepare_prob : std::false_type {};
template <typename Operation>
struct uring_prepare_prob<Operation, void_t<decltype(
    std::declval<Operation&>().prepare(std::declval<io_uring_sqe*>()))>>
    : conjunction<std::is_void<decltype(std::declval<Operation&>().prepare(std::declval<io_uring_sqe*>()))>,
          std::integral_constant<bool, noexcept(std::declval<Operation&>().prepare(std::declval<io_uring_sqe*>()))>> {};
} // namespace detail

// Borrow a host-owned ring. SQ preparation, CQ routing and detach belong to
// the host's serialized loop; close/status may be concurrent. The host reserves the top 16 user_data bits
// for this attachment and continues to own submit(), CQ advancement and exit().
class io_uring_context {
public:
    using callback = void (*)(void*, int, unsigned);

    explicit io_uring_context(io_uring& ring, std::uint16_t tag,
                              std::size_t capacity = 256)
        : ring_(&ring), tag_(tag), slots_(checked_capacity(capacity)) {
        for (std::size_t i = 0; i < slots_.size(); ++i)
            slots_[i].next = i + 1;
    }
    io_uring_context(const io_uring_context&) = delete;
    io_uring_context& operator=(const io_uring_context&) = delete;
    ~io_uring_context() noexcept {
        // An outstanding SQE/CQE still owns an awaitable reference.
        if (gate_.pending()) std::terminate();
    }

    // Success means staged in the SQ, not submitted to the kernel. In
    // particular a later short/failed io_uring_submit MUST NOT release it.
    template <typename Operation>
    int enqueue(Operation& operation, callback complete, void* data) noexcept {
        static_assert(detail::uring_prepare_prob<Operation>::value,
            "io_uring operation requirement: provide void prepare(io_uring_sqe*) noexcept, callable on an lvalue.");
        if (!complete) return -EINVAL;
        if (!gate_.try_acquire()) return -ESHUTDOWN;
        if (free_ == slots_.size()) { gate_.release(); return -EAGAIN; }
        auto* sqe = io_uring_get_sqe(ring_);
        if (!sqe) { gate_.release(); return -EAGAIN; }
        const auto index = free_;
        auto& slot = slots_[index];
        free_ = slot.next;
        ++slot.generation;
        slot.complete = complete;
        slot.data = data;
        operation.prepare(sqe);
        // One terminal CQE per request is required. No multishot, send_zc,
        // IOSQE_CQE_SKIP_SUCCESS, or linked requests in this initial adapter.
        if (sqe->flags & (IOSQE_CQE_SKIP_SUCCESS | IOSQE_IO_LINK | IOSQE_IO_HARDLINK | IOSQE_BUFFER_SELECT))
            std::terminate();
        io_uring_sqe_set_data64(sqe, (std::uint64_t(tag_) << 48)
            | (std::uint64_t(slot.generation) << 16) | index);
        return 0;
    }

    // Pass COPIED CQE fields after advancing the CQ. Returns false for foreign,
    // stale or duplicate tokens without interpreting them as pointers. The host
    // must reserve tag_ exclusively, including across attachment lifetimes.
    bool complete(std::uint64_t token, int result, unsigned flags) noexcept {
        if ((token >> 48) != tag_) return false;
        const auto index = static_cast<std::uint16_t>(token);
        if (index >= slots_.size()) return false;
        auto& slot = slots_[index];
        if (!slot.complete || slot.generation != std::uint32_t(token >> 16)) return false;
        if (flags & IORING_CQE_F_MORE) std::terminate();
        const auto fn = slot.complete;
        auto* data = slot.data;
        slot.complete = nullptr;
        slot.data = nullptr;
        // Never wrap a generation and accidentally accept an old token.
        if (slot.generation != UINT32_MAX) {
            slot.next = free_;
            free_ = index;
        }
        gate_.release(); // Original terminal CQE, never submit() or logical cancel.
        // Release the slot before invoking user code: continuations can submit
        // another operation or close/destroy this drained attachment.
        fn(data, result, flags);
        return true;
    }

    void close() noexcept { gate_.request_close(); }
    std::size_t pending() const noexcept { return gate_.pending(); }
    bool closed() const noexcept { return gate_.drained(); }
    bool try_detach() noexcept {
        close();
        if (!gate_.drained()) return false;
        ring_ = nullptr;
        return true;
    }

private:
    struct slot {
        callback complete = nullptr;
        void* data = nullptr;
        std::uint32_t generation = 0;
        std::size_t next = 0;
    };
    static std::size_t checked_capacity(std::size_t n) {
        if (n && n <= 65536) return n;
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        throw std::invalid_argument("io_uring_context capacity must be 1..65536");
#else
        std::terminate();
#endif
    }
    io_uring* ring_;
    std::uint16_t tag_;
    std::vector<slot> slots_;
    std::size_t free_ = 0;
    flux_foundry::detail::close_gate gate_;
};

template <typename Operation>
struct io_uring_request {
    io_uring_context* context;
    Operation operation;
};

template <typename Operation>
struct io_uring_result {
    Operation operation; // Keeps owned buffers alive and returns them to the flow.
    int result;
    unsigned flags;
};

template <typename Operation, typename E = flow_async_agg_err_t>
struct io_uring_awaitable final
    : awaitable_base<io_uring_awaitable<Operation, E>, io_uring_result<Operation>, E> {
    using async_result_type = result_t<io_uring_result<Operation>, E>;
    using request_type = io_uring_request<Operation>;
    static_assert(std::is_object<Operation>::value
            && !std::is_const<Operation>::value && !std::is_volatile<Operation>::value,
        "io_uring operation requirement: Operation must be an unqualified object type owned by the awaitable.");
    static_assert(detail::uring_prepare_prob<Operation>::value,
        "io_uring operation requirement: provide void prepare(io_uring_sqe*) noexcept, callable on an lvalue.");
    static_assert(std::is_nothrow_destructible<Operation>::value,
        "io_uring operation requirement: Operation must be nothrow destructible.");
    static_assert(std::is_nothrow_move_constructible<Operation>::value,
        "io_uring operation requirement: Operation must be nothrow move constructible.");
    static_assert(detail::io_error_prob<E>::value,
        "I/O awaitable requirement: specialize extension::detail::io_error<E> with static E make(int) noexcept.");

    explicit io_uring_awaitable(result_t<request_type, E>&& input) noexcept
        : request(std::move(input).value()) {}
    bool available() const noexcept { return request.context != nullptr; }
    int submit() noexcept {
        if (!available()) return -EINVAL;
        this->retain(); // Kernel/backend lifetime survives logical cancellation.
        const int rc = request.context->enqueue(request.operation, &on_complete, this);
        if (rc) this->release();
        return rc;
    }
    // Flow cancellation is logical. The host must still submit/drain the
    // original request; its owned buffers stay alive until the terminal CQE.
    void cancel() noexcept {}

private:
    request_type request;
    static void on_complete(void* data, int result, unsigned flags) noexcept {
        auto* self = static_cast<io_uring_awaitable*>(data);
        if (result < 0)
            self->resume(async_result_type(error_tag, detail::io_error<E>::make(-result)));
        else
            self->resume(async_result_type(value_tag,
                io_uring_result<Operation>{std::move(self->request.operation), result, flags}));
        self->release(); // Backend reference; resume may have consumed the flow reference.
    }
};

// Opt-in terminal-only lane. Accepted requests keep the fast awaitable's
// initial reference until the original terminal CQE. Controller cancellation
// is not registered, so no competing logical completion can release it early.
// The host must submit/drain accepted SQEs, including after close or a failed
// flush, exactly as for io_uring_awaitable. Never access self after resume().
template <typename Operation, typename E = flow_async_agg_err_t>
struct io_uring_fast_awaitable final
    : fast_awaitable_base<io_uring_fast_awaitable<Operation, E>, io_uring_result<Operation>, E> {
    using async_result_type = result_t<io_uring_result<Operation>, E>;
    using request_type = io_uring_request<Operation>;
    static_assert(std::is_object<Operation>::value
            && !std::is_const<Operation>::value && !std::is_volatile<Operation>::value,
        "io_uring operation requirement: Operation must be an unqualified object type owned by the awaitable.");
    static_assert(detail::uring_prepare_prob<Operation>::value,
        "io_uring operation requirement: provide void prepare(io_uring_sqe*) noexcept, callable on an lvalue.");
    static_assert(std::is_nothrow_destructible<Operation>::value,
        "io_uring operation requirement: Operation must be nothrow destructible.");
    static_assert(std::is_nothrow_move_constructible<Operation>::value,
        "io_uring operation requirement: Operation must be nothrow move constructible.");
    static_assert(detail::io_error_prob<E>::value,
        "I/O awaitable requirement: specialize extension::detail::io_error<E> with static E make(int) noexcept.");

    explicit io_uring_fast_awaitable(result_t<request_type, E>&& input) noexcept
        : request(std::move(input).value()) {}
    bool available() const noexcept { return request.context != nullptr; }
    int submit() noexcept {
        return available() ? request.context->enqueue(request.operation, &on_complete, this) : -EINVAL;
    }

private:
    request_type request;
    static void on_complete(void* data, int result, unsigned flags) noexcept {
        auto* self = static_cast<io_uring_fast_awaitable*>(data);
        if (result < 0)
            self->resume(async_result_type(error_tag, detail::io_error<E>::make(-result)));
        else
            self->resume(async_result_type(value_tag,
                io_uring_result<Operation>{std::move(self->request.operation), result, flags}));
    }
};

} // namespace extension

template <typename Operation, typename E = flow_async_agg_err_t>
auto await_io_uring() noexcept { return await<extension::io_uring_awaitable<Operation, E>>(); }

template <typename Operation, typename E = flow_async_agg_err_t, typename Executor>
auto await_io_uring(Executor&& executor) noexcept {
    return await<extension::io_uring_awaitable<Operation, E>>(std::forward<Executor>(executor));
}

template <typename Operation, typename E = flow_async_agg_err_t>
auto await_io_uring_fast() noexcept { return await<extension::io_uring_fast_awaitable<Operation, E>>(); }

template <typename Operation, typename E = flow_async_agg_err_t, typename Executor>
auto await_io_uring_fast(Executor&& executor) noexcept {
    return await<extension::io_uring_fast_awaitable<Operation, E>>(std::forward<Executor>(executor));
}
} // namespace flux_foundry
#endif
