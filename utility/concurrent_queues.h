#ifndef LITE_FNDS_LOCK_FREE_QUEUES_H
#define LITE_FNDS_LOCK_FREE_QUEUES_H

#include <atomic>
#include <thread>
#include "../base/traits.h"
#include "../memory/padded_t.h"
#include "../memory/inplace_t.h"
#include "back_off.h"

namespace lite_fnds {
template <typename T, size_t capacity>
struct spsc_queue {
    static_assert(std::is_nothrow_move_constructible<T>::value,
        "T must be nothrow move constructible");
    static_assert(capacity > 0 && (capacity & (capacity - 1)) == 0,
        "capacity must be power of 2");

protected:
    struct alignas(CACHE_LINE_SIZE) slot_t {
        std::atomic<uint32_t> ready;
        raw_inplace_storage_base<T> storage;

        slot_t() noexcept : ready { 0 } { }

        T& data() noexcept {
            return *storage.ptr();
        }

        void destroy() noexcept {
            storage.destroy();
        }
    };

    padded_t<size_t, CACHE_LINE_SIZE> _h { 0 };
    padded_t<size_t, CACHE_LINE_SIZE> _t { 0 };

    slot_t _data[capacity];
public:
    spsc_queue() noexcept :
        _h { 0 } , _t { 0 } {
    }

    spsc_queue(const spsc_queue&) = delete;
    spsc_queue(spsc_queue&& q) noexcept = delete;
    spsc_queue& operator=(const spsc_queue&) = delete;
    spsc_queue& operator=(spsc_queue&&) = delete;

    ~spsc_queue() noexcept  {
        while (_h != _t) {
            auto& slot = _data[_h & (capacity - 1)];
            if (slot.ready.load(std::memory_order_relaxed)) {
                slot.destroy();
                slot.ready.store(0, std::memory_order_relaxed);
            }
            ++_h;
        }
    }

   template <typename T_, typename... Args,
        std::enable_if_t<std::is_nothrow_constructible<T_, Args&&...>::value>* = nullptr>
    bool try_emplace(Args&&... args) noexcept {
       auto& slot = this->_data[_t & (capacity - 1)];        // full
       if (slot.ready.load(std::memory_order_acquire)) {
           return false;
       }
       slot.storage.construct(std::forward<Args>(args)...);
       slot.ready.store(1, std::memory_order_release);
       ++_t;
       return true;
    }

#if LFNDS_HAS_EXCEPTIONS
    template <typename T_, typename... Args,
        std::enable_if_t <conjunction_v<
        negation<std::is_nothrow_constructible<T_, Args&&...>>, std::is_constructible<T_, Args&&...>>>* = nullptr>
    bool try_emplace(Args&&... args) noexcept(std::is_nothrow_constructible<T_, Args&&...>::value) {
        T tmp(std::forward<Args>(args)...);
        return try_emplace(std::move(tmp));
    }
#endif

    bool try_emplace(T&& object) noexcept {
        auto& slot = this->_data[_t & (capacity - 1)];
        // full
        if (slot.ready.load(std::memory_order_acquire)) {
            return false;
        }
        slot.storage.construct(std::move(object));
        slot.ready.store(1, std::memory_order_release);
        ++_t;
        return true;
    }

#if LFNDS_HAS_EXCEPTIONS
    template <typename T_, typename ... Args,
        typename = std::enable_if_t<std::is_constructible<T_, Args&&...>::value>>
    void wait_and_emplace(Args&&... args)
        noexcept(std::is_nothrow_constructible<T_, Args&&...>::value) {
        T tmp(std::forward<Args>(args)...);
        wait_and_emplace(std::move(tmp));
    }
#endif

    void wait_and_emplace(T&& object) noexcept {
        backoff_strategy<> backoff;
        for (;; backoff.yield()) {
            auto& slot = this->_data[_t & (capacity - 1)];
            // full
            if (slot.ready.load(std::memory_order_acquire)) {
                continue;
            }
            slot.storage.construct(std::move(object));
            slot.ready.store(1, std::memory_order_release);
            ++_t;
            return;
        }
    }

    inplace_t<T> try_pop() noexcept {
        inplace_t<T> res;
        auto& slot = this->_data[_h & (capacity - 1)];
        if (!slot.ready.load(std::memory_order_acquire)) {
            return res;
        }

        res.emplace(std::move(slot.data()));
        slot.destroy();
        slot.ready.store(0, std::memory_order_release);
        _h++;
        return res;
    }

    T wait_and_pop() noexcept {
        backoff_strategy<> backoff;
        for (;; backoff.yield()) {
            auto& slot = this->_data[_h & (capacity - 1)];
            if (!slot.ready.load(std::memory_order_acquire)) {
                continue;
            }

            T tmp(std::move(slot.data()));
            slot.destroy();
            slot.ready.store(0, std::memory_order_release);
            _h++;
            return tmp;
        }
    }
};

template <typename T, size_t capacity>
struct mpsc_queue {
    static_assert(std::is_nothrow_move_constructible<T>::value,
        "T must be nothrow move constructible");
    static_assert(std::is_nothrow_destructible<T>::value,
        "T must be nothrow destructible");
    static_assert(capacity > 0 && (capacity & (capacity - 1)) == 0,
        "capacity must be power of 2");

    using value_type = T;
protected:
    static constexpr size_t MASK = capacity - 1;

    struct alignas(CACHE_LINE_SIZE) slot_t {
        std::atomic<uint32_t> ready;
        raw_inplace_storage_base<T> storage;

        slot_t() noexcept : ready { 0 } { }

        T& data() noexcept {
            return *storage.ptr();
        }

        void destroy() noexcept {
            storage.destroy();
        }
    };

    padded_t<size_t, CACHE_LINE_SIZE> _h { 0 };
    padded_t<std::atomic<size_t>, CACHE_LINE_SIZE> _t { 0 };

    alignas(CACHE_LINE_SIZE) slot_t _data[capacity];

    static_assert(alignof(slot_t) == CACHE_LINE_SIZE, "slot_t must be cache-line aligned");
public:
    mpsc_queue() = default;

    mpsc_queue(const mpsc_queue&) = delete;
    mpsc_queue(mpsc_queue&& q) noexcept = delete;
    mpsc_queue& operator=(const mpsc_queue&) = delete;
    mpsc_queue& operator=(mpsc_queue&&) = delete;

    ~mpsc_queue() noexcept {
        auto& t_ = _t.get();
        const size_t t = t_.load(std::memory_order_relaxed);
        while (_h != t) {
            slot_t& s = _data[_h & MASK];
            if (s.ready.load(std::memory_order_relaxed)) {
                s.destroy();
                s.ready.store(0, std::memory_order_relaxed);
            }
            ++_h;
        }
    }

    template <typename T_, typename... Args,
        std::enable_if_t<std::is_nothrow_constructible<T_, Args&&...>::value>* = nullptr>
    bool try_emplace(Args&& ... args) noexcept {
        auto& t_ = _t.get();

        size_t t = t_.load(std::memory_order_relaxed);
        slot_t &slot = _data[t & MASK];
        if (slot.ready.load(std::memory_order_acquire) == 0
            && t_.compare_exchange_strong(t, t + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
            slot.storage.construct(std::forward<Args>(args)...);
            slot.ready.store(1, std::memory_order_release);
            return true;
        }

        return false;
    }

#if LFNDS_HAS_EXCEPTIONS
    template <typename T_, typename... Args,
        std::enable_if_t<conjunction_v<
            negation<std::is_nothrow_constructible<T_, Args&&...>>, std::is_constructible<T_, Args&&...>>>* = nullptr>
    bool try_emplace(Args&&... args)
        noexcept(std::is_nothrow_constructible<T_, Args&&...>::value) {
        T tmp(std::forward<Args>(args)...);
        return try_emplace(std::move(tmp));
    }
#endif

    bool try_emplace(T&& object) noexcept {
        constexpr int max_retry = 8;
        auto& t_ = _t.get();

        size_t t = t_.load(std::memory_order_relaxed);

        slot_t &slot = _data[t & MASK];
        if (slot.ready.load(std::memory_order_acquire) == 0
            && t_.compare_exchange_strong(t, t + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
            slot.storage.construct(std::move(object));
            slot.ready.store(1, std::memory_order_release);
            return true;
        }

        return false;
    }

    template <typename T_ = T, typename... Args,
        std::enable_if_t<std::is_nothrow_constructible<T_, Args&&...>::value>* = nullptr>
    void wait_and_emplace(Args&&... args) noexcept {
        auto& t_ = _t.get();
        backoff_strategy<> backoff;
        for (;; backoff.yield()) {
            size_t t = t_.load(std::memory_order_relaxed);

            slot_t& slot = _data[t & MASK];
            if (slot.ready.load(std::memory_order_acquire) == 0
                && t_.compare_exchange_weak(t, t + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
                slot.storage.construct(std::forward<Args>(args)...);
                slot.ready.store(1, std::memory_order_release);
                return;
            }
        }
    }

#if LFNDS_HAS_EXCEPTIONS
    template <typename T_, typename... Args,
        std::enable_if_t<conjunction_v<
            negation<std::is_nothrow_constructible<T_, Args&&...>>,
            std::is_constructible<T_, Args&&...>>>* = nullptr>
    void wait_and_emplace(Args&&... args)
        noexcept(std::is_nothrow_constructible<T, Args&&...>::value) {
        T_ tmp(std::forward<Args>(args)...);
        wait_and_emplace(std::move(tmp));
    }
#endif

    void wait_and_emplace(T&& object) noexcept {
        auto& t_ = _t.get();
        backoff_strategy<> backoff;
        for (;; backoff.yield()) {
            size_t t = t_.load(std::memory_order_relaxed);

            slot_t& slot = _data[t & MASK];
            if (slot.ready.load(std::memory_order_acquire) == 0
                && t_.compare_exchange_weak(t, t + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
                slot.storage.construct(std::move(object));
                slot.ready.store(1, std::memory_order_release);
                return;
            }
        }
    }

    inplace_t<T> try_pop() noexcept {
        inplace_t<T> res;

        slot_t& slot = this->_data[_h & MASK];
        if (!slot.ready.load(std::memory_order_acquire)) {
            return res;
        }

        res.emplace(std::move(slot.data()));
        slot.destroy();
        slot.ready.store(0, std::memory_order_release);
        ++_h;
        return res;
    }

    T wait_and_pop() noexcept {
        backoff_strategy<> backoff;
        for (;; backoff.yield()) {
            slot_t& slot = this->_data[_h & MASK];
            if (!slot.ready.load(std::memory_order_acquire)) {
                continue;
            }

            T tmp = std::move(slot.data());
            slot.destroy();
            slot.ready.store(0, std::memory_order_release);
            ++_h;
            return tmp;
        }
    }

    // this should only be called in consumer thread (otherwise UB)
    size_t size() const noexcept {
        auto& t_ = _t.get();
        return t_.load(std::memory_order_relaxed) - _h;
    }
};

template <typename T, unsigned long capacity>
struct mpmc_queue {
private:
    static_assert(conjunction_v<std::is_nothrow_move_constructible<T>, std::is_nothrow_destructible<T>>,
        "T should be nothrow move constructible and nothrow destructible.");
    static_assert(capacity > 0 && (capacity & (capacity - 1)) == 0,
        "capacity must be power of 2");

    struct alignas(CACHE_LINE_SIZE) slot_t {
        std::atomic<size_t> sequence;
        raw_inplace_storage_base<T> storage;

        slot_t() : sequence(0) {
        }

        ~slot_t() noexcept {
            if (sequence.load(std::memory_order_relaxed) & 1) {
                destroy();
            }
        }

        T& data() noexcept {
            return *storage.ptr();
        }

        void destroy() noexcept {
            storage.destroy();
        }
    };

    alignas(CACHE_LINE_SIZE) slot_t m_q[capacity];

    padded_t<std::atomic<size_t>, CACHE_LINE_SIZE> _h { 0 };
    padded_t<std::atomic<size_t>, CACHE_LINE_SIZE> _t { 0 };

    static constexpr unsigned long bit_msk = capacity - 1;

public:
    using value_type = T;
    mpmc_queue() :
        m_q {}, _h { 0 }, _t { 0 } {
    }

    ~mpmc_queue() = default;
    mpmc_queue(const mpmc_queue&) = delete;
    mpmc_queue(mpmc_queue&& q) noexcept = delete;
    mpmc_queue& operator=(const mpmc_queue&) = delete;
    mpmc_queue& operator=(mpmc_queue&&) = delete;

    void wait_and_emplace(T&& obj) noexcept {
        auto& t_ = _t.get();
        backoff_strategy<> backoff;
        for (;; backoff.yield()) {
            auto i = t_.load(std::memory_order_relaxed);
            auto& slot = this->m_q[i & bit_msk];
            auto seq = slot.sequence.load(std::memory_order_acquire), _seq = (i / capacity) << 1;
            if (seq == _seq
                && t_.compare_exchange_weak(i, i + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
                slot.storage.construct(std::move(obj));
                slot.sequence.store(seq + 1, std::memory_order_release);
                return;
            }
        }
    }

    template <typename T_ = T, typename... Args,
        std::enable_if_t<std::is_nothrow_constructible<T_, Args&&...>::value>* = nullptr>
    void wait_and_emplace(Args&&... args) noexcept {
        auto& t_ = _t.get();
        backoff_strategy<> backoff;
        for (;; backoff.yield()) {
            auto i = t_.load(std::memory_order_relaxed);
            auto& slot = this->m_q[i & bit_msk];
            auto seq = slot.sequence.load(std::memory_order_acquire), _seq = (i / capacity) << 1;
            if (seq == _seq
                && t_.compare_exchange_weak(i, i + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
                slot.storage.construct(std::forward<Args>(args)...);
                slot.sequence.store(seq + 1, std::memory_order_release);
                return;
            }
        }
    }

#if LFNDS_HAS_EXCEPTIONS
    template <typename T_, typename... Args,
        std::enable_if_t<conjunction_v<
            negation<std::is_nothrow_constructible<T_, Args&&...>>,
            std::is_constructible<T_, Args&&...>>>* = nullptr>
    void wait_and_emplace(Args&&... args)
        noexcept(std::is_nothrow_constructible<T_, Args&&...>::value) {
        T tmp(std::forward<Args>(args)...);
        wait_and_emplace(std::move(tmp));
    }
#endif

    T wait_and_pop() noexcept {
        auto& h_ = _h.get();
        backoff_strategy<> backoff;
        for (;; backoff.yield()) {
            auto i = h_.load(std::memory_order_relaxed);
            auto& slot = m_q[i & bit_msk];
            auto _seq = slot.sequence.load(std::memory_order_acquire), seq = ((i / capacity) << 1) + 1;
            // try to claim this slot
            if (_seq == seq
                && h_.compare_exchange_weak(i, i + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
                auto ret = std::move(slot.data());
                slot.destroy();
                slot.sequence.store(seq + 1, std::memory_order_release);
                return ret;
            }
        }
    }

    bool try_emplace(T&& obj) noexcept {
        auto& t_ = _t.get();
        auto& h_ = _h.get();
        auto i = t_.load(std::memory_order_relaxed);
        auto& slot = m_q[i & bit_msk];
        auto _seq = slot.sequence.load(std::memory_order_acquire), seq = (i / capacity) << 1;

        // full
        if ((ptrdiff_t)(_seq - seq) < 0) {
            return false;
        }
        // try to claim this slot
        if (_seq == seq && t_.compare_exchange_strong(i, i + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
            slot.storage.construct(std::move(obj));
            slot.sequence.store(seq + 1, std::memory_order_release);
            return true;
        }
        return false;
    }

    template <typename T_ = T, typename... Args,
        std::enable_if_t<std::is_nothrow_constructible<T_, Args&&...>::value>* = nullptr>
    bool try_emplace(Args&&... args) noexcept {
        auto& t_ = _t.get();
        auto& h_ = _h.get();
        auto i = t_.load(std::memory_order_relaxed);
        auto& slot = m_q[i & bit_msk];
        auto _seq = slot.sequence.load(std::memory_order_acquire), seq = (i / capacity) << 1;

        // full
        if ((ptrdiff_t)(_seq - seq) < 0) {
            return false;
        }
        // try to claim this slot
        if (_seq == seq && t_.compare_exchange_strong(i, i + 1,
            std::memory_order_relaxed, std::memory_order_relaxed)) {
            slot.storage.construct(std::forward<Args>(args)...);
            slot.sequence.store(seq + 1, std::memory_order_release);
            return true;
        }
        return false;
    }

#if LFNDS_HAS_EXCEPTIONS
    template <typename T_, typename... Args,
        std::enable_if_t<conjunction_v<
            negation<std::is_nothrow_constructible<T_, Args&&...>>,
            std::is_constructible<T_, Args&&...>>>* = nullptr>
    bool try_emplace(Args&&... args)
        noexcept(std::is_nothrow_constructible<T, Args&&...>::value) {
        T tmp(std::forward<Args>(args)...);
        return try_emplace(std::move(tmp));
    }
#endif

    inplace_t<T> try_pop() noexcept {
        auto& h_ = _h.get();
        inplace_t<T> res;

        auto i = h_.load(std::memory_order_relaxed);
        auto& slot = m_q[i & bit_msk];
        auto _seq = slot.sequence.load(std::memory_order_acquire), seq = ((i / capacity) << 1) + 1;

        if ((ptrdiff_t)(_seq - seq) < 0) {
            return res;
        }

        if (_seq == seq && h_.compare_exchange_strong(i, i + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
            res.emplace(std::move(slot.data()));
            slot.destroy();
            slot.sequence.store(seq + 1, std::memory_order_release);
            return res;
        }

        return res;
    }

    // only for approximating the size
    size_t size() const noexcept {
        auto& t_ = _t.get();
        auto& h_ = _h.get();
        return t_.load(std::memory_order_relaxed) - h_.load(std::memory_order_relaxed);
    }

    // only for approximating the queue is empty
    bool empty() noexcept {
        return size() == 0;
    }
};

template <typename T, size_t capacity>
struct spmc_deque {
    static_assert(conjunction_v<std::is_nothrow_move_constructible<T>, std::is_nothrow_destructible<T>>,
        "T should be nothrow move constructible and nothrow destructible.");
    static_assert(capacity > 0 && (capacity & (capacity - 1)) == 0,
        "capacity must be power of 2");

    // bits [0:1]: state
    enum STATE {
        STATE_MASK = 0b11,
        ST_EMPTY = 0b00,
        ST_SHARED = 0b01,
        ST_PRIVATE = 0b10,
        ST_CLAIMED = 0b11,
        POS_SHIFT = 2,
    };

    struct alignas(CACHE_LINE_SIZE) slot_t {
        // | 64 ... 2 | 1 ｜ 0 |
        // | -------- | --- ｜  ---------- |
        // | round no.| locked by owner ｜  Empty/Full |

        std::atomic<size_t> sequence;
        raw_inplace_storage_base<T> storage;

        slot_t() : sequence(0) {
        }

        ~slot_t() noexcept {
            if (sequence.load(std::memory_order_relaxed) & STATE_MASK) {
                destroy();
            }
        }

        T &data() noexcept {
            return *storage.ptr();
        }

        void destroy() noexcept {
            storage.destroy();
        }
    };

    alignas(CACHE_LINE_SIZE) slot_t m_q[capacity];

    padded_t<std::atomic<size_t>, CACHE_LINE_SIZE> _h{0};
    padded_t<size_t, CACHE_LINE_SIZE> _t{0};

    std::thread::id _tid;

    static constexpr unsigned long bit_msk = capacity - 1;

    bool is_owner() noexcept {
        return std::this_thread::get_id() == _tid;
    }

    static size_t make_sequence(size_t t, STATE st) noexcept {
        return ((t / capacity) << 2) | st;
    }

public:
    spmc_deque() noexcept
        : _tid(std::this_thread::get_id()) {
    }

    ~spmc_deque() noexcept = default;

    // this should only be called by the owner thread;
    bool try_emplace_back(T &&obj) noexcept {
        if (!is_owner()) {
            return false;
        }

        auto &t_ = _t.get();

        auto &slot = m_q[t_ & bit_msk];
        // sequence the slot exactly has, and the seq we expect it should be
        auto _seq = slot.sequence.load(std::memory_order_acquire), seq = make_sequence(t_, ST_EMPTY);

        // queue is full
        if (_seq != seq) {
            return false;
        }

        // 1. mark t_ - 1 as shared, memory_order_release is to publish the slot to consumers.
        auto expected = make_sequence(t_ - 1, ST_PRIVATE), desired = make_sequence(t_ - 1, ST_SHARED);
        m_q[(t_ - 1) & bit_msk].sequence.compare_exchange_strong(expected, desired,
                                                                 std::memory_order_release, std::memory_order_relaxed);

        // 2. emplace_back
        ++t_;
        slot.storage.construct(std::move(obj));
        // mark the newly emplaced slot as private, since the consumers can not touch this slot,
        // if they see this slot is private, so, nothing should be synchronized with seq, relaxed is pretty fine.
        slot.sequence.store(make_sequence(t_ - 1, ST_PRIVATE), std::memory_order_relaxed);
        return true;
    }

    template<typename T_ = T, typename... Args,
        std::enable_if_t<std::is_nothrow_constructible<T_, Args &&...>::value>* = nullptr>
    bool try_emplace(Args &&... args) noexcept {
        if (!is_owner()) {
            return false;
        }

        auto &t_ = _t.get();

        auto &slot = m_q[t_ & bit_msk];
        // sequence the slot exactly has, and the seq we expect it should be
        auto _seq = slot.sequence.load(std::memory_order_acquire), seq = make_sequence(t_, ST_EMPTY);

        // queue is full
        if (_seq != seq) {
            return false;
        }

        // 1. mark t_ - 1 as shared, memory_order_release is to publish the slot to consumers.
        auto expected = make_sequence(t_ - 1, ST_PRIVATE), desired = make_sequence(t_ - 1, ST_SHARED);
        m_q[(t_ - 1) & bit_msk].sequence.compare_exchange_strong(expected, desired,
                                                                 std::memory_order_release, std::memory_order_relaxed);

        // 2. emplace_back
        ++t_;
        slot.storage.construct(std::forward<Args>(args)...);
        // mark the newly emplaced slot as private, since the consumers can not touch this slot,
        // if they see this slot is private, so, nothing should be synchronized with seq, relaxed is pretty fine.
        slot.sequence.store(make_sequence(t_ - 1, ST_PRIVATE), std::memory_order_relaxed);
        return true;
    }

#if LFNDS_HAS_EXCEPTIONS
    template<typename T_, typename... Args,
        std::enable_if_t<conjunction_v<
            negation<std::is_nothrow_constructible<T_, Args &&...> >,
            std::is_constructible<T_, Args &&...> > >* = nullptr>
    bool try_emplace(Args &&... args) {
        if (!is_owner()) {
            return false;
        }

        T obj(std::forward<Args>(args)...);
        return this->try_emplace(std::move(obj));
    }
#endif

    inplace_t<T> try_pop_back() noexcept {
        if (!is_owner()) {
            return {};
        }

        auto &t_ = _t.get();
        auto &slot = m_q[(t_ - 1) & bit_msk];
        auto _seq = slot.sequence.load(std::memory_order_acquire);

#define mark_as_private(t) do {                                                                                        \
        size_t expected = make_sequence((t) - 1, ST_SHARED);                                                           \
        m_q[((t) - 1) & bit_msk].sequence.compare_exchange_strong(expected, make_sequence((t) - 1, ST_PRIVATE),        \
                                                                 std::memory_order_relaxed, std::memory_order_relaxed);\
} while (0)
        inplace_t<T> res;
        if (_seq == make_sequence(t_ - 1, ST_PRIVATE)) {
            res.emplace(std::move(slot.data()));
            slot.destroy();
            slot.sequence.store(make_sequence(--t_, ST_EMPTY), std::memory_order_relaxed);
            // try to mark slot[t - 1] as private,
            // if the cas success, we successfully marked the slot as private
            // else the slot is stolen by consumers, nothing will happen
            mark_as_private(t_);
        } else if (_seq == make_sequence(t_ - 1, ST_SHARED)) {
            if (slot.sequence.compare_exchange_strong(_seq, make_sequence(t_ - 1, ST_EMPTY),
                                                      std::memory_order_relaxed, std::memory_order_relaxed)) {
                res.emplace(std::move(slot.data()));
                slot.destroy();
                slot.sequence.store(make_sequence(--t_, ST_EMPTY), std::memory_order_relaxed);
                // try to mark slot[t - 1] as private,
                // if the cas success, we successfully marked the slot as private
                // else the slot is stolen by consumers, nothing will happen
                mark_as_private(t_);
            }
        }
#undef mark_as_private
        return res;
    }

    inplace_t<T> try_pop_front() noexcept {
        if (is_owner()) {
            return {};
        }

        auto &h_ = _h.get();
        auto p = h_.load(std::memory_order_acquire);

        auto &slot = m_q[p & bit_msk];
        auto seq = make_sequence(p, ST_SHARED);

        if (slot.sequence.compare_exchange_strong(seq, make_sequence(p, ST_CLAIMED),
                                                  std::memory_order_acquire, std::memory_order_relaxed)) {
            h_.fetch_add(1, std::memory_order_relaxed);
            inplace_t<T> res(std::move(slot.data()));
            slot.destroy();
            slot.sequence.store(make_sequence(p + capacity, ST_EMPTY), std::memory_order_release);
            return res;
        }

        return {};
    }
};
}

#endif
