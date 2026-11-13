#ifndef FLUX_FOUNDRY_IDENTITY_POOL_H
#define FLUX_FOUNDRY_IDENTITY_POOL_H

#include <atomic>
#include <limits>
#include <thread>
#include "pooling.h"

namespace flux_foundry {

// Headerless storage: the caller keeps the allocation identity separately.
// Only dead storage is used for intrusive remote-return links. Each TLS pool
// owns its inbox; stable registry slots admit publishers before exposing it.
template <size_t Size, size_t Align, size_t Cache = detail::flux_foundry_default_cache_cap, size_t Batch = 16,
          size_t Slots = 64, size_t Remote = Cache>
class identity_pool_allocator {
    struct node { node* next; };
    struct cache;
    static_assert(Size >= sizeof(node), "identity pool requires space for a free-list link");
    static_assert(Align >= alignof(node) && (Align & (Align - 1)) == 0,
        "identity pool requires power-of-two alignment sufficient for a free-list link");
    static_assert(Size <= std::numeric_limits<size_t>::max() - (Align - 1),
        "identity pool allocation size must not overflow");
    static_assert(Cache > 0 && Batch > 0 && Slots > 0 && Slots < 256 && Remote >= Batch,
        "identity pool requires nonzero cache/batch/slots, slots < 256 and remote capacity >= batch");
    static constexpr int counter_lock_free =
        std::is_same<size_t, unsigned int>::value ? ATOMIC_INT_LOCK_FREE :
        std::is_same<size_t, unsigned long>::value ? ATOMIC_LONG_LOCK_FREE :
        std::is_same<size_t, unsigned long long>::value ? ATOMIC_LLONG_LOCK_FREE :
        std::is_same<size_t, unsigned short>::value ? ATOMIC_SHORT_LOCK_FREE : 0;
    static_assert(counter_lock_free == 2 && ATOMIC_POINTER_LOCK_FREE == 2,
        "identity pool requires always-lock-free size_t and pointer atomics");

    // One CAS domain for epoch, admission and close. A saturated epoch is
    // never reused. A saturated admission count falls back to local adoption.
    static constexpr size_t closed = 1, occupied = 2, ticket = 4;
    static constexpr size_t epoch_shift = 16;
    static constexpr size_t low_mask = (size_t{1} << epoch_shift) - 1;
    static constexpr size_t ticket_mask = low_mask & ~(closed | occupied);
    static constexpr size_t max_epoch = std::numeric_limits<size_t>::max() >> epoch_shift;
    static constexpr bool pooled = Align <= alignof(std::max_align_t) &&
        detail::alloc_size(Size, Align) <= detail::pool_max_block_size;

    struct slot {
        std::atomic<size_t> state{closed};
        cache* target = nullptr; // Published by opening; protected by tickets.
    };
    static slot* registry() noexcept {
        static slot slots[Slots];
        return slots;
    }
    struct thread_view { cache* current = nullptr; bool retired = false; };
    static thread_view& view() noexcept {
        static thread_local thread_view v;
        return v;
    }

    static void* backing_alloc(std::true_type) noexcept {
        if (auto p = detail::get_pool().allocate(detail::alloc_size(Size, Align))) return p;
        return std::malloc(detail::alloc_size(Size, Align));
    }
    static void* backing_alloc(std::false_type) noexcept {
        return flux_foundry::aligned_alloc(Align, detail::alloc_size(Size, Align));
    }
    static void backing_free(void* p, std::true_type) noexcept {
        auto& pool = detail::get_pool();
        if (pool.belong_to(p)) pool.deallocate(p);
        else std::free(p);
    }
    static void backing_free(void* p, std::false_type) noexcept { aligned_free(p); }
    static void backing_free(void* p) noexcept {
        backing_free(p, std::integral_constant<bool, pooled>{});
    }

    static size_t register_pool(cache* target) noexcept {
        auto* table = registry();
        for (size_t i = 0; i < Slots; ++i) {
            auto before = table[i].state.load(std::memory_order_acquire);
            if ((before & low_mask) != closed || (before >> epoch_shift) == max_epoch) continue;
            const auto epoch = (before >> epoch_shift) + 1;
            const auto reserved = (epoch << epoch_shift) | occupied | closed;
            if (!table[i].state.compare_exchange_strong(before, reserved,
                    std::memory_order_acq_rel, std::memory_order_acquire)) continue;
            table[i].target = target;
            table[i].state.store(reserved & ~closed, std::memory_order_release);
            return (epoch << 8) | (i + 1);
        }
        return 0; // Registry exhaustion preserves allocation correctness.
    }
    static slot* acquire(size_t identity) noexcept {
        const auto index = identity & 255;
        if (!index || index > Slots) return nullptr;
        auto* entry = registry() + index - 1;
        const auto epoch = identity >> 8;
        auto before = entry->state.load(std::memory_order_acquire);
        for (;;) {
            if ((before >> epoch_shift) != epoch || (before & closed) ||
                !(before & occupied) || (before & ticket_mask) == ticket_mask) return nullptr;
            if (entry->state.compare_exchange_weak(before, before + ticket,
                    std::memory_order_acq_rel, std::memory_order_acquire)) return entry;
        }
    }
    static void release(slot* entry) noexcept {
        entry->state.fetch_sub(ticket, std::memory_order_release);
    }

    struct cache {
        void* local[Cache];
        size_t top = 0;
        std::atomic<node*> incoming{nullptr};
        std::atomic<size_t> incoming_count{0}; // Includes unpublished reservations.
        node* outgoing = nullptr;
        node* outgoing_tail = nullptr;
        size_t outgoing_count = 0, outgoing_identity = 0;
        size_t identity;

        cache() noexcept : identity(register_pool(this)) {
            // Ensure backing storage is initialized before this TLS destructor.
            (void)detail::get_pool();
            view().current = this;
        }
        void put(void* p, bool keep = true) noexcept {
            if (keep && top < Cache) local[top++] = p;
            else backing_free(p);
        }
        size_t adopt(node* list, bool keep) noexcept {
            size_t count = 0;
            while (list) {
                auto* next = list->next;
                list->~node();
                put(list, keep);
                list = next;
                ++count;
            }
            return count;
        }
        void collect(bool keep) noexcept {
            auto* list = incoming.exchange(nullptr, std::memory_order_acquire);
            const auto count = adopt(list, keep);
            if (count) incoming_count.fetch_sub(count, std::memory_order_release);
        }
        void flush(bool keep = true) noexcept {
            if (!outgoing) return;
            auto* first = outgoing;
            auto* last = outgoing_tail;
            const auto count = outgoing_count;
            const auto origin = outgoing_identity;
            outgoing = outgoing_tail = nullptr;
            outgoing_count = 0;
            auto* entry = acquire(origin);
            if (entry) {
                auto* owner = entry->target;
                auto used = owner->incoming_count.load(std::memory_order_acquire);
                while (used <= Remote - count) {
                    if (owner->incoming_count.compare_exchange_weak(used, used + count,
                            std::memory_order_acq_rel, std::memory_order_acquire)) {
                        auto* head = owner->incoming.load(std::memory_order_relaxed);
                        do { last->next = head; }
                        while (!owner->incoming.compare_exchange_weak(head, first,
                            std::memory_order_release, std::memory_order_relaxed));
                        // Published nodes may already be reused. Only the
                        // stable slot may be touched after publication.
                        release(entry);
                        return;
                    }
                }
                release(entry);
            }
            // Closed/stale/full destinations transfer free storage to this
            // thread. A future allocation stamps this thread's identity.
            adopt(first, keep);
        }
        void remote_free(size_t origin, void* p) noexcept {
            if (outgoing && origin != outgoing_identity) flush();
            auto* link = ::new (p) node{nullptr};
            if (outgoing_tail) outgoing_tail->next = link;
            else outgoing = link;
            outgoing_tail = link;
            outgoing_identity = origin;
            if (++outgoing_count == Batch) flush();
        }
        ~cache() noexcept {
            // Later TLS destructors cannot resurrect or access this cache.
            view().retired = true;
            view().current = nullptr;
            flush(false);
            slot* entry = nullptr;
            size_t state = 0;
            if (identity) {
                entry = registry() + (identity & 255) - 1;
                entry->state.fetch_or(closed, std::memory_order_acq_rel);
                // Wait only for publishers already admitted, never for live
                // awaitables or unflushed batches on other threads.
                do {
                    state = entry->state.load(std::memory_order_acquire);
                    if (state & ticket_mask) std::this_thread::yield();
                } while (state & ticket_mask);
            }
            collect(false); // Last drain occurs AFTER all publishers leave.
            while (top) backing_free(local[--top]);
            if (entry) {
                entry->target = nullptr;
                entry->state.store(state & ~occupied, std::memory_order_release);
            }
        }
    };
    static cache* current() noexcept {
        auto& v = view();
        if (v.retired) return nullptr;
        if (!v.current) {
            static thread_local cache c;
            (void)c;
        }
        return v.current;
    }

public:
    using identity_type = size_t;
    static identity_type current_identity() noexcept {
        auto* c = current();
        return c ? c->identity : 0;
    }
    static void* alloc() noexcept {
        if (auto* c = current()) {
            if (!c->top) c->collect(true);
            if (c->top) return c->local[--c->top];
        }
        return backing_alloc(std::integral_constant<bool, pooled>{});
    }
    static void dealloc(identity_type origin, void* p) noexcept {
        if (!p) return;
        if (auto* c = current()) {
            if (!origin || origin == c->identity) c->put(p);
            else c->remote_free(origin, p);
        } else backing_free(p);
    }
    // Optional processing-boundary flush for partial batches. TLS exit also
    // flushes; an idle live sender otherwise retains at most Batch-1 blocks.
    static void flush_remote_returns() noexcept {
        if (auto* c = view().current) c->flush();
    }
};

template <typename T, size_t Cache = FLUX_FOUNDRY_AWAITABLE_POOL_SLOT_COUNT>
struct identity_pooling_base {
private:
    // The identity belongs to the live object, never to an allocation header.
    size_t origin_;
    template <typename U>
    using allocator_for = identity_pool_allocator<sizeof(U), alignof(U), Cache>;
public:
    identity_pooling_base() noexcept : origin_(allocator_for<T>::current_identity()) {}
    identity_pooling_base(const identity_pooling_base&) = delete;
    identity_pooling_base& operator=(const identity_pooling_base&) = delete;
    static constexpr size_t align() noexcept {
        return alignof(T) > alignof(std::max_align_t) ? alignof(T) : alignof(std::max_align_t);
    }
    static constexpr size_t alloc_size() noexcept { return detail::alloc_size(sizeof(T), align()); }
    static void* operator new(size_t n, const std::nothrow_t&) noexcept {
        static_assert(std::is_final<T>::value, "identity-pooled awaitables must be final");
        return n == sizeof(T) ? allocator_for<T>::alloc() : nullptr;
    }
    static void* operator new(size_t n) {
        auto* p = operator new(n, std::nothrow);
        if (!p) {
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            throw std::bad_alloc();
#else
            std::abort();
#endif
        }
        return p;
    }
    // Constructor failure / explicit delete: the object identity cannot be
    // read after destruction. Safely adopt on the deleting thread instead.
    static void operator delete(void* p) noexcept {
        allocator_for<T>::dealloc(allocator_for<T>::current_identity(), p);
    }
    static void operator delete(void* p, const std::nothrow_t&) noexcept { operator delete(p); }
    static void operator delete(void* p, size_t) noexcept { operator delete(p); }
    static void* operator new[](size_t) = delete;
    static void operator delete[](void*) = delete;
    static void flush_remote_returns() noexcept { allocator_for<T>::flush_remote_returns(); }
    static void destroy_pooled(T* p) noexcept {
        static_assert(std::is_nothrow_destructible<T>::value,
            "identity-pooled awaitables must be nothrow destructible");
        static_assert(static_cast<void* (*)(size_t)>(&T::operator new) ==
                      static_cast<void* (*)(size_t)>(&identity_pooling_base::operator new),
            "identity-pooled awaitables must use the inherited allocation functions");
        static_assert(static_cast<void* (*)(size_t, const std::nothrow_t&)>(&T::operator new) ==
                      static_cast<void* (*)(size_t, const std::nothrow_t&)>(&identity_pooling_base::operator new),
            "identity-pooled awaitables must use the inherited allocation functions");
        static_assert(static_cast<void (*)(void*)>(&T::operator delete) ==
                      static_cast<void (*)(void*)>(&identity_pooling_base::operator delete),
            "identity-pooled awaitables must use the inherited deallocation functions");
        const auto origin = p->origin_;
        p->~T();
        allocator_for<T>::dealloc(origin, p);
    }
};
} // namespace flux_foundry
#endif
