#ifndef FLUX_FOUNDRY_DPDK_AWAITABLE_H
#define FLUX_FOUNDRY_DPDK_AWAITABLE_H

#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <type_traits>
#include <utility>
#include "../flow/flow_awaitable.h"
#include "../flow/flow_node.h"
#include "detail/io_error.h"

namespace flux_foundry {
namespace extension {

struct dpdk_api {
    using packet_type = rte_mbuf;
    static void free(packet_type* packet) noexcept { rte_pktmbuf_free(packet); }
    static std::uint16_t rx(std::uint16_t port, std::uint16_t queue,
                            packet_type** packets, std::uint16_t capacity) noexcept {
        return rte_eth_rx_burst(port, queue, packets, capacity);
    }
    static std::uint16_t tx(std::uint16_t port, std::uint16_t queue,
                            packet_type** packets, std::uint16_t count) noexcept {
        return rte_eth_tx_burst(port, queue, packets, count);
    }
};

namespace detail {
template <typename Api, typename = void>
struct dpdk_packet_type_prob : std::false_type { using type = void; };
template <typename Api>
struct dpdk_packet_type_prob<Api, void_t<typename Api::packet_type>>
    : conjunction<std::is_object<typename Api::packet_type>,
          negation<std::is_const<typename Api::packet_type>>,
          negation<std::is_volatile<typename Api::packet_type>>> {
    // A fallback avoids pointer-to-reference errors hiding the contract message.
    using type = std::conditional_t<dpdk_packet_type_prob::value, typename Api::packet_type, void>;
};
template <typename Api>
using dpdk_packet_t = typename dpdk_packet_type_prob<Api>::type;

template <typename Api, typename = void>
struct dpdk_free_prob : std::false_type {};
template <typename Api>
struct dpdk_free_prob<Api, void_t<decltype(Api::free(std::declval<dpdk_packet_t<Api>*>()))>>
    : conjunction<std::is_void<decltype(Api::free(std::declval<dpdk_packet_t<Api>*>()))>,
          std::integral_constant<bool, noexcept(Api::free(std::declval<dpdk_packet_t<Api>*>()))>> {};

template <typename Api, typename = void>
struct dpdk_rx_prob : std::false_type {};
template <typename Api>
struct dpdk_rx_prob<Api, void_t<decltype(Api::rx(std::declval<std::uint16_t>(), std::declval<std::uint16_t>(),
    std::declval<dpdk_packet_t<Api>**>(), std::declval<std::uint16_t>()))>>
    : conjunction<std::is_same<std::uint16_t, decltype(Api::rx(std::declval<std::uint16_t>(),
          std::declval<std::uint16_t>(), std::declval<dpdk_packet_t<Api>**>(), std::declval<std::uint16_t>()))>,
          std::integral_constant<bool, noexcept(Api::rx(std::declval<std::uint16_t>(),
          std::declval<std::uint16_t>(), std::declval<dpdk_packet_t<Api>**>(), std::declval<std::uint16_t>()))>> {};

template <typename Api, typename = void>
struct dpdk_tx_prob : std::false_type {};
template <typename Api>
struct dpdk_tx_prob<Api, void_t<decltype(Api::tx(std::declval<std::uint16_t>(), std::declval<std::uint16_t>(),
    std::declval<dpdk_packet_t<Api>**>(), std::declval<std::uint16_t>()))>>
    : conjunction<std::is_same<std::uint16_t, decltype(Api::tx(std::declval<std::uint16_t>(),
          std::declval<std::uint16_t>(), std::declval<dpdk_packet_t<Api>**>(), std::declval<std::uint16_t>()))>,
          std::integral_constant<bool, noexcept(Api::tx(std::declval<std::uint16_t>(),
          std::declval<std::uint16_t>(), std::declval<dpdk_packet_t<Api>**>(), std::declval<std::uint16_t>()))>> {};
} // namespace detail

// A burst owns every mbuf still in its array, including chained segments via
// rte_pktmbuf_free. Moving a burst transfers that ownership without allocation.
template <std::size_t Capacity = 32, typename Api = dpdk_api>
class dpdk_packet_batch {
    static_assert(Capacity > 0 && Capacity <= std::numeric_limits<std::uint16_t>::max(),
        "DPDK batch requirement: Capacity must be in 1..65535, representable by the burst API.");
    static_assert(detail::dpdk_packet_type_prob<Api>::value,
        "DPDK API requirement: packet_type must name an unqualified object type.");
    static_assert(detail::dpdk_free_prob<Api>::value,
        "DPDK API requirement: provide static void free(packet_type*) noexcept.");
    static_assert(detail::dpdk_rx_prob<Api>::value,
        "DPDK API requirement: provide static uint16_t rx(uint16_t, uint16_t, packet_type**, uint16_t) noexcept.");
    static_assert(detail::dpdk_tx_prob<Api>::value,
        "DPDK API requirement: provide static uint16_t tx(uint16_t, uint16_t, packet_type**, uint16_t) noexcept.");
public:
    using packet_type = detail::dpdk_packet_t<Api>;
    dpdk_packet_batch() noexcept = default;
    dpdk_packet_batch(const dpdk_packet_batch&) = delete;
    dpdk_packet_batch& operator=(const dpdk_packet_batch&) = delete;
    dpdk_packet_batch(dpdk_packet_batch&& other) noexcept : count_(other.count_) {
        for (std::uint16_t i = 0; i < count_; ++i) packets_[i] = other.packets_[i];
        other.count_ = 0;
    }
    dpdk_packet_batch& operator=(dpdk_packet_batch&& other) noexcept {
        if (this != &other) {
            clear();
            count_ = other.count_;
            for (std::uint16_t i = 0; i < count_; ++i) packets_[i] = other.packets_[i];
            other.count_ = 0;
        }
        return *this;
    }
    ~dpdk_packet_batch() noexcept { clear(); }
    std::uint16_t size() const noexcept { return count_; }
    packet_type* operator[](std::uint16_t i) const noexcept { return packets_[i]; }
    void clear() noexcept {
        for (std::uint16_t i = 0; i < count_; ++i) Api::free(packets_[i]);
        count_ = 0;
    }
    static dpdk_packet_batch receive(std::uint16_t port, std::uint16_t queue) noexcept {
        dpdk_packet_batch batch;
        batch.count_ = Api::rx(port, queue, batch.packets_.data(), static_cast<std::uint16_t>(Capacity));
        return batch;
    }
    // TX burst reports PMD acceptance, not delivery on the wire. Only accepted
    // mbufs leave our ownership; the unsent tail remains available for retry.
    std::uint16_t transmit(std::uint16_t port, std::uint16_t queue) noexcept {
        if (!count_) return 0;
        const auto sent = Api::tx(port, queue, packets_.data(), count_);
        for (std::uint16_t i = sent; i < count_; ++i) packets_[i - sent] = packets_[i];
        count_ -= sent;
        return sent;
    }
private:
    // Only [0, count_) is initialized and owned. RX writes that prefix;
    // moves must not read the unused tail of a partial burst.
    std::array<packet_type*, Capacity> packets_;
    std::uint16_t count_ = 0;
};

// Attach to a configured, started host RX queue. One pending burst waiter per
// attachment; the host loop calls poll(). EAL, ports, queues and mempools remain
// host-owned. All methods except awaitable cancellation are loop-thread-only.
template <std::size_t Capacity = 32, typename Api = dpdk_api>
class dpdk_rx_context {
public:
    using batch_type = dpdk_packet_batch<Capacity, Api>;
    static_assert(std::is_nothrow_move_constructible<batch_type>::value
            && std::is_nothrow_destructible<batch_type>::value,
        "DPDK context requirement: packet batches must be nothrow movable and destructible.");
    using callback = void (*)(void*, batch_type&&, int);
    using cancel_probe = bool (*)(void*);
    dpdk_rx_context(std::uint16_t port, std::uint16_t queue) noexcept : port_(port), queue_(queue) {}
    dpdk_rx_context(const dpdk_rx_context&) = delete;
    dpdk_rx_context& operator=(const dpdk_rx_context&) = delete;
    ~dpdk_rx_context() noexcept { if (complete_) std::terminate(); }

    int enqueue(callback complete, cancel_probe canceled, void* data) noexcept {
        if (closing_) return -ESHUTDOWN;
        if (complete_) return -EAGAIN;
        if (!complete || !canceled) return -EINVAL;
        complete_ = complete;
        canceled_ = canceled;
        data_ = data;
        return 0;
    }
    // At most one burst per call. Empty RX is pending, not EOF or failure.
    // A canceled waiter is drained even when the NIC has no incoming packets.
    bool poll() noexcept {
        if (!complete_) return false;
        const bool aborted = closing_ || canceled_(data_);
        auto batch = aborted ? batch_type{} : batch_type::receive(port_, queue_);
        if (!aborted && !batch.size()) return false;
        const auto fn = complete_;
        auto* data = data_;
        complete_ = nullptr;
        canceled_ = nullptr;
        data_ = nullptr;
        fn(data, std::move(batch), aborted ? ECANCELED : 0);
        return true;
    }
    void close() noexcept { closing_ = true; }
    bool pending() const noexcept { return complete_ != nullptr; }
    bool closed() const noexcept { return closing_ && !pending(); }
    bool try_detach() noexcept { close(); return !pending(); }
private:
    std::uint16_t port_, queue_;
    callback complete_ = nullptr;
    cancel_probe canceled_ = nullptr;
    void* data_ = nullptr;
    bool closing_ = false;
};

template <std::size_t Capacity = 32, typename E = flow_async_agg_err_t, typename Api = dpdk_api>
struct dpdk_rx_awaitable final
    : awaitable_base<dpdk_rx_awaitable<Capacity, E, Api>, dpdk_packet_batch<Capacity, Api>, E> {
    using context_type = dpdk_rx_context<Capacity, Api>;
    using batch_type = typename context_type::batch_type;
    using async_result_type = result_t<batch_type, E>;
    static_assert(detail::io_error_prob<E>::value,
        "I/O awaitable requirement: specialize extension::detail::io_error<E> with static E make(int) noexcept.");
    explicit dpdk_rx_awaitable(result_t<context_type*, E>&& input) noexcept : context(input.value()) {}
    bool available() const noexcept { return context != nullptr; }
    int submit() noexcept {
        if (!available()) return -EINVAL;
        this->retain();
        const int rc = context->enqueue(&on_complete, &is_canceled, this);
        if (rc) this->release();
        return rc;
    }
    void cancel() noexcept { canceled.store(true, std::memory_order_release); }
private:
    context_type* context;
    std::atomic<bool> canceled{false};
    static bool is_canceled(void* data) noexcept {
        return static_cast<dpdk_rx_awaitable*>(data)->canceled.load(std::memory_order_acquire);
    }
    static void on_complete(void* data, batch_type&& batch, int error) noexcept {
        auto* self = static_cast<dpdk_rx_awaitable*>(data);
        if (error) self->resume(async_result_type(error_tag, detail::io_error<E>::make(error)));
        else self->resume(async_result_type(value_tag, std::move(batch)));
        self->release();
    }
};

} // namespace extension

template <std::size_t Capacity = 32, typename E = flow_async_agg_err_t>
auto await_dpdk_rx() noexcept { return await<extension::dpdk_rx_awaitable<Capacity, E>>(); }

template <std::size_t Capacity = 32, typename E = flow_async_agg_err_t, typename Executor>
auto await_dpdk_rx(Executor&& executor) noexcept {
    return await<extension::dpdk_rx_awaitable<Capacity, E>>(std::forward<Executor>(executor));
}
} // namespace flux_foundry
#endif
