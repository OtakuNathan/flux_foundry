#ifndef FLUX_FOUNDRY_CLOSE_GATE_H
#define FLUX_FOUNDRY_CLOSE_GATE_H

#include <atomic>
#include <cassert>
#include <cstddef>
#include <limits>
#include <type_traits>

namespace flux_foundry { namespace detail {

// The same admission protocol as simple_executor: a stop bit and outstanding
// tickets share one CAS domain. No reopen/reset; the owner drains asynchronously.
class close_gate {
    // C++14 has no atomic<T>::is_always_lock_free. Select the standard macro
    // for size_t's actual type, not merely another type with the same width.
    static constexpr int counter_lock_free =
        std::is_same<std::size_t, unsigned int>::value ? ATOMIC_INT_LOCK_FREE :
        std::is_same<std::size_t, unsigned long>::value ? ATOMIC_LONG_LOCK_FREE :
        std::is_same<std::size_t, unsigned long long>::value ? ATOMIC_LLONG_LOCK_FREE :
        std::is_same<std::size_t, unsigned short>::value ? ATOMIC_SHORT_LOCK_FREE :
        std::is_same<std::size_t, unsigned char>::value ? ATOMIC_CHAR_LOCK_FREE : 0;
    static_assert(counter_lock_free == 2,
        "close_gate requirement: std::atomic<size_t> must be always lock-free on this target.");
    static constexpr std::size_t stop = 1;
    static constexpr std::size_t unit = 2;
    std::atomic<std::size_t> state_{0};
public:
    close_gate() noexcept = default;
    bool try_acquire() noexcept {
        auto state = state_.load(std::memory_order_acquire);
        for (;;) {
            // Keep one spare ticket for the closer's final notification.
            if ((state & stop) || state > std::numeric_limits<std::size_t>::max() - 2 * unit)
                return false;
            if (state_.compare_exchange_weak(state, state + unit,
                    std::memory_order_acq_rel, std::memory_order_acquire)) return true;
        }
    }
    // Only the winner may send a close notification. If it needs to touch a
    // handle, acquire that final ticket AT THE SAME TIME as setting stop.
    bool request_close(bool notification_ticket = false) noexcept {
        auto state = state_.load(std::memory_order_acquire);
        for (;;) {
            if (state & stop) return false;
            const auto next = (state | stop) + (notification_ticket ? unit : 0);
            if (state_.compare_exchange_weak(state, next,
                    std::memory_order_acq_rel, std::memory_order_acquire)) return true;
        }
    }
    void release() noexcept {
        const auto before = state_.fetch_sub(unit, std::memory_order_acq_rel);
        assert(before >= unit && "unbalanced close ticket");
        (void)before;
    }
    bool closing() const noexcept { return (state_.load(std::memory_order_acquire) & stop) != 0; }
    bool drained() const noexcept { return state_.load(std::memory_order_acquire) == stop; }
    std::size_t pending() const noexcept { return state_.load(std::memory_order_acquire) / unit; }
};

}} // namespace flux_foundry::detail
#endif
