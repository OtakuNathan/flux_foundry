#ifndef LITE_FNDS_LOCK_FREE_QUEUES_H
#define LITE_FNDS_LOCK_FREE_QUEUES_H

#include <atomic>
#include <thread>
#include "../base/traits.h"
#include "../memory/inplace_t.h"
#include "yield.h"

namespace lite_fnds {
template <typename T, size_t capacity>
struct spsc_queue {
    static_assert(std::is_nothrow_move_constructible<T>::value, 
        "T must be nothrow move constructible");
    static_assert((capacity & (capacity - 1)) == 0, "capacity must be power of 2");

protected:
    using slot_t = raw_inplace_storage_base<T>;
    slot_t _data[capacity];

    alignas(CACHE_LINE_SIZE) std::atomic<size_t> _h { 0 };
    pad_t<sizeof(_h)> _pad1;

    alignas(CACHE_LINE_SIZE) std::atomic<size_t> _t { 0 };
    pad_t<sizeof(_t)> _pad2;

public:
    spsc_queue() noexcept :
        _h { 0 } , _t { 0 } {
    }

    ~spsc_queue() noexcept  {
        std::size_t h = _h.load(std::memory_order_relaxed);
        const std::size_t t = _t.load(std::memory_order_relaxed);
        while (h != t) {
            this->_data[h & (capacity - 1)].destroy();
            ++h;
        }
    }

   template <typename T_, typename... Args,
        std::enable_if_t<std::is_nothrow_constructible<T_, Args&&...>::value>* = nullptr>
    bool try_emplace(Args&&... args) noexcept {
       auto t = _t.load(std::memory_order_relaxed);
       auto h = _h.load(std::memory_order_acquire);
       if (t - h == capacity) {
           return false;
       }

       auto& slot = this->_data[t & (capacity - 1)];
       slot.construct(std::forward<Args>(args)...);
       _t.store(t + 1, std::memory_order_release);
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
        auto t = _t.load(std::memory_order_relaxed);
        auto h = _h.load(std::memory_order_acquire);
        if (t - h == capacity) {
            return false;
        }

        auto& slot = this->_data[t & (capacity - 1)];
        slot.construct(std::move(object));
        _t.store(t + 1, std::memory_order_release);
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
        for (;;yield()) {
            auto t = _t.load(std::memory_order_relaxed);
            auto h = _h.load(std::memory_order_acquire);
            if (t - h == capacity) {
                continue;
            }

            auto& slot = this->_data[t & (capacity - 1)];
            slot.construct(std::move(object));
            _t.store(t + 1, std::memory_order_release);
            break;
        }
    }

    inplace_t<T> try_pop() noexcept {
        inplace_t<T> res;
        auto h = _h.load(std::memory_order_relaxed);
        auto t = _t.load(std::memory_order_acquire);
        if (h == t) {
            return res;
        }

        auto& slot = this->_data[h & (capacity - 1)];
        res.emplace(std::move(*slot.ptr()));
        slot.destroy();

        _h.store(h + 1, std::memory_order_release);
        return res;
    }

    T wait_and_pop() noexcept {
        for (;;yield()) {
            auto h = _h.load(std::memory_order_relaxed);
            auto t = _t.load(std::memory_order_acquire);
            if (h == t) {
                continue;
            }

            auto& slot = this->_data[h & (capacity - 1)];
            auto tmp = std::move(*slot.ptr());
            slot.destroy();

            _h.store(h + 1, std::memory_order_release);
            return tmp;
        }
    }

    size_t size() const noexcept {
        return _t.load(std::memory_order_relaxed) - _h.load(std::memory_order_relaxed);
    }
};

template <typename T, size_t capacity>
struct mpsc_queue {
    static_assert(std::is_nothrow_move_constructible<T>::value,
        "T must be nothrow move constructible");
    static_assert(std::is_nothrow_destructible<T>::value,
        "T must be nothrow destructible");
    static_assert((capacity & (capacity - 1)) == 0,
        "capacity must be power of 2");

protected:
    static constexpr size_t MASK = capacity - 1;
    using value_type = T;

    struct alignas(CACHE_LINE_SIZE) slot_t {
        std::atomic<uint8_t> ready;
        raw_inplace_storage_base<T> storage;

        slot_t() noexcept : ready { 0 } {
        }

        T& data() noexcept { 
            return *storage.ptr(); 
        }

        void destroy() noexcept { 
            storage.destroy(); 
        }
    };
    
    alignas(CACHE_LINE_SIZE) slot_t _data[capacity];

    alignas(CACHE_LINE_SIZE) std::atomic<size_t> _h { 0 };
    pad_t<sizeof(_h)> _pad1;

    alignas(CACHE_LINE_SIZE) std::atomic<size_t> _t { 0 };
    pad_t<sizeof(_t)> _pad2;

public:
    mpsc_queue() = default;

    ~mpsc_queue() noexcept {
        size_t h = _h.load(std::memory_order_relaxed);
        const size_t t = _t.load(std::memory_order_relaxed);
        while (h != t) {
            slot_t& s = _data[h & MASK];
            if (s.ready.load(std::memory_order_relaxed)) {
                s.destroy();
                s.ready.store(0, std::memory_order_relaxed);
            }
            ++h;
        }
    }

    template <typename T_, typename... Args,
        std::enable_if_t<std::is_nothrow_constructible<T_, Args&&...>::value>* = nullptr>
    bool try_emplace(Args&& ... args) noexcept {
        constexpr int max_retry = 8;

        for (int attempt = 0; attempt < max_retry; ++attempt) {
            size_t t = _t.load(std::memory_order_relaxed);
            size_t h = _h.load(std::memory_order_acquire);

            if (t - h == capacity) {
                return false;
            }

            if (_t.compare_exchange_weak(t, t + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
                slot_t& slot = _data[t & MASK];
                slot.storage.construct(std::forward<Args>(args)...);
                slot.ready.store(1, std::memory_order_release);
                return true;
            }

            yield();
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

        for (int attempt = 0; attempt < max_retry; ++attempt) {
            size_t t = _t.load(std::memory_order_relaxed);
            size_t h = _h.load(std::memory_order_acquire);

            if (t - h == capacity) {
                return false;
            }

            if (_t.compare_exchange_weak(t, t + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
                slot_t& slot = _data[t & MASK];
                slot.storage.construct(std::move(object));
                slot.ready.store(1, std::memory_order_release);
                return true;
            }
            yield();
        }
        return false;
    }

    template <typename T_ = T, typename... Args,
        std::enable_if_t<std::is_nothrow_constructible<T_, Args&&...>::value>* = nullptr>
    void wait_and_emplace(Args&&... args) noexcept {
        for (;; yield()) {
            size_t t = _t.load(std::memory_order_relaxed);
            size_t h = _h.load(std::memory_order_acquire);

            if (t - h == capacity) {
                continue;
            }

            if (_t.compare_exchange_weak(t, t + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
                slot_t& slot = _data[t & MASK];
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
        for (;; yield()) {
            size_t t = _t.load(std::memory_order_relaxed);
            size_t h = _h.load(std::memory_order_acquire);

            if (t - h == capacity) {
                continue;
            }

            if (_t.compare_exchange_weak(t, t + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
                slot_t& slot = _data[t & MASK];
                slot.storage.construct(std::move(object));
                slot.ready.store(1, std::memory_order_release);
                return;
            }
        }
    }

    inplace_t<T> try_pop() noexcept {
        inplace_t<T> res;
        size_t h = _h.load(std::memory_order_relaxed);
        size_t t = _t.load(std::memory_order_relaxed);
        if (h == t) {
            return res;
        }

        slot_t& slot = this->_data[h & MASK];
        if (!slot.ready.load(std::memory_order_acquire)) {
            return res;
        }

        res.emplace(std::move(slot.data()));
        slot.destroy();
        slot.ready.store(0, std::memory_order_relaxed);
        _h.store(h + 1, std::memory_order_release);
        return res;
    }

    T wait_and_pop() noexcept {
        for (;;) {
            size_t h = _h.load(std::memory_order_relaxed);

            slot_t& slot = this->_data[h & MASK];
            if (!slot.ready.load(std::memory_order_acquire)) {
                yield();
                continue;
            }

            T tmp = std::move(slot.data());
            slot.destroy();
            slot.ready.store(0, std::memory_order_relaxed);
            _h.store(h + 1, std::memory_order_release);
            return tmp;
        }
    }

    size_t size() const noexcept {
        return _t.load(std::memory_order_relaxed) - _h.load(std::memory_order_relaxed);
    }
};

template <typename T, unsigned long capacity>
struct mpmc_queue {
private:
    static_assert(conjunction_v<std::is_nothrow_move_constructible<T>, std::is_nothrow_destructible<T>>, 
        "T should be nothrow move constructible and nothrow destructible.");
    static_assert(!(capacity & (capacity - 1)), "capacity shouble be power of 2");

    struct alignas(CACHE_LINE_SIZE) slot_t {
        std::atomic<size_t> sequence;
        raw_inplace_storage_base<T> storage;

        slot_t() : sequence(0) {
        }

        ~slot_t() noexcept {
            if (sequence & 1) {
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

    alignas(CACHE_LINE_SIZE) std::atomic<size_t> _h { 0 };
    pad_t<sizeof(_h)> _pad1;

    alignas(CACHE_LINE_SIZE) std::atomic<size_t> _t { 0 };
    pad_t<sizeof(_t)> _pad2;

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
        for (;; yield()) {
            auto i = _t.load(std::memory_order_relaxed);
            auto& slot = this->m_q[i & bit_msk];
            auto seq = slot.sequence.load(std::memory_order_acquire), _seq = (i / capacity) << 1;
            if (seq == _seq
                && _t.compare_exchange_weak(i, i + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
                slot.storage.construct(std::move(obj));
                slot.sequence.store(seq + 1, std::memory_order_release);
                return;
            }
        }
    }


    template <typename T_ = T, typename... Args,
        std::enable_if_t<std::is_nothrow_constructible<T_, Args&&...>::value>* = nullptr>
    void wait_and_emplace(Args&&... args) noexcept {
        for (;; yield()) {
            auto i = _t.load(std::memory_order_relaxed);
            auto& slot = this->m_q[i & bit_msk];
            auto seq = slot.sequence.load(std::memory_order_acquire), _seq = (i / capacity) << 1;
            if (seq == _seq
                && _t.compare_exchange_weak(i, i + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
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
        for (;;yield()) {
            auto i = _h.load(std::memory_order_relaxed);
            auto& slot = m_q[i & bit_msk];
            auto _seq = slot.sequence.load(std::memory_order_acquire), seq = ((i / capacity) << 1) + 1;
            // try to claim this slot
            if (_seq == seq
                && _h.compare_exchange_weak(i, i + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
                auto ret = std::move(slot.data());
                slot.destroy();
                slot.sequence.store(seq + 1, std::memory_order_release);
                return ret;
            }
        }
    }

    bool try_emplace(T&& obj) noexcept {
        auto i = _t.load(std::memory_order_relaxed);
        auto& slot = m_q[i & bit_msk];
        auto _seq = slot.sequence.load(std::memory_order_acquire), seq = (i / capacity) << 1;

        // full
        if ((ptrdiff_t)(_seq - seq) < 0) {
            return false;
        }
        // try to claim this slot
        if (_seq == seq && _t.compare_exchange_strong(i, i + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
            slot.storage.construct(std::move(obj));
            slot.sequence.store(seq + 1, std::memory_order_release);
            return true;
        }
        return false;
    }


    template <typename T_ = T, typename... Args,
        std::enable_if_t<std::is_nothrow_constructible<T_, Args&&...>::value>* = nullptr>
    bool try_emplace(Args&&... args) noexcept {
        auto i = _t.load(std::memory_order_relaxed);
        auto& slot = m_q[i & bit_msk];
        auto _seq = slot.sequence.load(std::memory_order_acquire), seq = (i / capacity) << 1;

        // full
        if ((ptrdiff_t)(_seq - seq) < 0) {
            return false;
        }
        // try to claim this slot
        if (_seq == seq && _t.compare_exchange_strong(i, i + 1, 
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

    inplace_t<T> try_pop() noexcept {
        inplace_t<T> res;

        auto i = _h.load(std::memory_order_relaxed);
        auto& slot = m_q[i & bit_msk];
        auto _seq = slot.sequence.load(std::memory_order_acquire), seq = ((i / capacity) << 1) + 1;

        if ((ptrdiff_t)(_seq - seq) < 0) {
            return res;
        }

        if (_seq == seq && _h.compare_exchange_weak(i, i + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
            res.emplace(std::move(slot.data()));
            slot.destroy();
            slot.sequence.store(seq + 1, std::memory_order_release);
            return res;
        }

        return res;
    }
#endif

    // only for approximating the size
    size_t size() const noexcept {
        return _t.load(std::memory_order_relaxed) - _h.load(std::memory_order_relaxed);
    }

    // only for approximating the queue is empty
    bool empty() noexcept {
        return size() == 0;
    }
};

}

#endif
