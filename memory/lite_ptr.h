//
// Created by Nathan on 08/01/2026.
//

#ifndef LF_TEST_REF_COUNT_H
#define LF_TEST_REF_COUNT_H

#include <atomic>
#include <stdexcept>
#include <cassert>
#include <new>
#include <memory>

#include "../base/inplace_base.h"
#include "flat_storage.h"
#include "aligned_alloc.h"

namespace flux_foundry {

    template <typename T>
    struct default_deleter {
        void operator()(T* ptr) noexcept {
            if (ptr) {
                ptr->~T();
            }
        }
    };

    using in_place_t = in_place_index<0>;
    constexpr static in_place_index<0> in_place;

    // THREAD SAFETY GUARANTEES (same as std::shared_ptr):
    // - Multiple lite_ptr instances pointing to the same object can be used
    //   concurrently from different threads (refcount is atomic)
    // - Concurrent access to the SAME lite_ptr instance requires external
    //   synchronization (the cb pointer itself is NOT atomic)
    // - If you need atomic operations on the pointer itself, use std::atomic<lite_ptr>
    //   or external synchronization

    template <typename T,
              typename F = default_deleter<T>,
              size_t align = alignof(T),
              typename A = aligned_malloc_allocator>
    class lite_ptr {
        static_assert(align >= alignof(T), "align must be >= alignof(T)");
        static_assert((align & (align - 1)) == 0, "align must be power of two");
        static_assert(std::is_copy_constructible<A>::value,
            "allocator type must be copy constructible");

        using op = raw_inplace_storage_base<T>;

        struct cb_t {
            alignas(align) unsigned char data[sizeof(T)];
            alignas(CACHE_LINE_SIZE) compressed_pair<std::atomic<size_t>, F> cb;
            A alloc;

            template <typename G, typename AllocLike, typename ... Args>
            cb_t(G&& g, AllocLike&& alloc_, Args&& ... args)
                noexcept(conjunction_v<std::is_nothrow_constructible<T, Args&&...>,
                        std::is_nothrow_constructible<F, G&&>,
                        std::is_nothrow_constructible<A, AllocLike&&>>)
                : cb(1, std::forward<G>(g)), alloc(std::forward<AllocLike>(alloc_)) {
                op::construct_at(data, std::forward<Args>(args)...);
            }

            ~cb_t() noexcept {
                cb.second()(reinterpret_cast<T*>(data));
            }
        };

        struct guard {
            cb_t* cb;
            A* alloc;
            guard(cb_t* cb_, A* alloc_) noexcept
                : cb(cb_), alloc(alloc_) {
            }
            ~guard() noexcept {
                if (cb && alloc) {
                    alloc->deallocate(cb);
                }
            }
        };

        cb_t* cb;

        static constexpr size_t alloc_size() noexcept {
            return (sizeof(cb_t) + alignof(cb_t) - 1) & ~(alignof(cb_t) - 1);
        }

        template <typename AllocLike, typename G, typename ... Args>
        void emplace_with_allocator(AllocLike&& alloc_like, G&& g, Args&& ... args)
#if !FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            noexcept
#endif
        {
            A alloc(std::forward<AllocLike>(alloc_like));
            auto cb_ = static_cast<cb_t*>(alloc.allocate(alignof(cb_t), alloc_size()));
            if (!cb_) {
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
                throw std::bad_alloc();
#else
                return;
#endif
            }

            guard g_(cb_, &alloc);
            new (g_.cb) cb_t(std::forward<G>(g), alloc, std::forward<Args>(args)...);
            cb = g_.cb;
            g_.cb = nullptr;
        }

    public:
        using element_type = T;
        using allocator_type = A;
        static constexpr size_t required_allocation_size() noexcept {
            return alloc_size();
        }
        static constexpr size_t required_allocation_align() noexcept {
            return alignof(cb_t);
        }

        lite_ptr() noexcept : cb(nullptr) {}

        template <typename ... Args, typename F_ = F,
                std::enable_if_t<conjunction_v<std::is_same<F_, default_deleter<T>>,
#if FLUEX_FOUNDRY_HAS_EXCEPTIONS
                        std::is_constructible<T, Args&&...>
#else
                        std::is_nothrow_constructible<T, Args&&...>,
                        std::is_nothrow_constructible<A>
#endif
        >>* = nullptr>
        explicit lite_ptr(in_place_t, Args&& ... args)
#if !FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            noexcept
#endif
            : cb {nullptr} {
            emplace_with_allocator(A{}, default_deleter<T>(), std::forward<Args>(args)...);
        }

        template <typename AllocLike, typename ... Args, typename F_ = F,
                std::enable_if_t<conjunction_v<std::is_same<F_, default_deleter<T>>,
#if FLUEX_FOUNDRY_HAS_EXCEPTIONS
                        std::is_constructible<A, AllocLike&&>, std::is_constructible<T, Args&&...>
#else
                        std::is_nothrow_constructible<A, AllocLike&&>, std::is_nothrow_constructible<T, Args&&...>
#endif
        >>* = nullptr>
        explicit lite_ptr(in_place_t, std::allocator_arg_t, AllocLike&& alloc, Args&& ... args)
#if !FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            noexcept
#endif
            : cb {nullptr} {
            emplace_with_allocator(std::forward<AllocLike>(alloc), default_deleter<T>(), std::forward<Args>(args)...);
        }

        template <typename G, typename ... Args, typename F_ = F,
                std::enable_if_t<conjunction_v<
                        negation<std::is_same<F_, default_deleter<T>>>,
#if FLUEX_FOUNDRY_HAS_EXCEPTIONS
                        std::is_constructible<F_, G&&>, std::is_constructible<T, Args&&...>
#else
                        std::is_nothrow_constructible<F_, G&&>, std::is_nothrow_constructible<T, Args&&...>,
                        std::is_nothrow_constructible<A>
#endif
        >>* = nullptr>
        lite_ptr(in_place_t, G&& g, Args&& ... args)
#if !FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            noexcept
#endif
            : cb {nullptr} {
            emplace_with_allocator(A{}, std::forward<G>(g), std::forward<Args>(args)...);
        }

        template <typename AllocLike, typename G, typename ... Args, typename F_ = F,
                std::enable_if_t<conjunction_v<
                        negation<std::is_same<F_, default_deleter<T>>>,
#if FLUEX_FOUNDRY_HAS_EXCEPTIONS
                        std::is_constructible<A, AllocLike&&>,
                        std::is_constructible<F_, G&&>, std::is_constructible<T, Args&&...>
#else
                        std::is_nothrow_constructible<A, AllocLike&&>,
                        std::is_nothrow_constructible<F_, G&&>, std::is_nothrow_constructible<T, Args&&...>
#endif
        >>* = nullptr>
        lite_ptr(in_place_t, std::allocator_arg_t, AllocLike&& alloc, G&& g, Args&& ... args)
#if !FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            noexcept
#endif
            : cb {nullptr} {
            emplace_with_allocator(std::forward<AllocLike>(alloc), std::forward<G>(g), std::forward<Args>(args)...);
        }

        lite_ptr(const lite_ptr& rhs) noexcept : cb{rhs.cb} {
            rhs.retain();
        }

        lite_ptr(lite_ptr&& rhs) noexcept : cb{rhs.cb} {
            rhs.cb = nullptr;
        }

        lite_ptr& operator=(const lite_ptr& rhs) noexcept {
            if (this != &rhs) {
                release();
                rhs.retain();
                if (rhs.cb) {
                    cb = rhs.cb;
                }
            }
            return *this;
        }

        lite_ptr& operator=(lite_ptr&& rhs) noexcept {
            if (this != &rhs) {
                release();
                cb = rhs.cb;
                rhs.cb = nullptr;
            }
            return *this;
        }

        T* get() const noexcept {
            return cb ? reinterpret_cast<T*>(cb->data) : nullptr;
        }

        explicit operator bool() const noexcept {
            return cb != nullptr;
        }

        void swap(lite_ptr& rhs) noexcept {
            using std::swap;
            swap(cb, rhs.cb);
        }

        ~lite_ptr() noexcept {
            release();
        }

        T& operator*() const noexcept {
            assert(cb && "attempting to dereferencing nullptr");
            return *reinterpret_cast<T*>(cb->data);
        }

        T* operator->() const noexcept {
            assert(cb && "attempting to accessing nullptr");
            return reinterpret_cast<T*>(cb->data);
        }

        void release() noexcept {
            auto ccb = cb;
            cb = nullptr;
            if (ccb) {
#if FLUEX_FOUNDRY_WITH_TSAN
                UNLIKELY_IF(ccb->cb.first().fetch_sub(1, std::memory_order_acq_rel) == 1) {
#else
                UNLIKELY_IF(ccb->cb.first().fetch_sub(1, std::memory_order_release) == 1) {
                    std::atomic_thread_fence(std::memory_order_acquire);
#endif
                    auto alloc = ccb->alloc;
                    ccb->~cb_t();
                    alloc.deallocate(ccb);
                }
            }
        }

        void retain() const noexcept {
            if (cb) {
                cb->cb.first().fetch_add(1, std::memory_order_relaxed);
            }
        }

        size_t use_count() const noexcept {
            return !cb ? 0 : cb->cb.first().load(std::memory_order_relaxed);
        }
    };

    template <typename T, typename F, size_t align, typename A>
    void swap(lite_ptr<T, F, align, A>& lhs , lite_ptr<T, F, align, A>& rhs) noexcept {
        lhs.swap(rhs);
    }

    template <typename T, typename ... Args,
            std::enable_if_t<std::is_constructible<lite_ptr<T>, in_place_t, Args&&...>::value>* = nullptr>
    lite_ptr<T> make_lite_ptr(Args&&... args)
        noexcept(std::is_nothrow_constructible<lite_ptr<T>, in_place_t, Args&&...>::value) {
        return lite_ptr<T>(in_place, std::forward<Args>(args)...);
    }

    template <typename T, typename Alloc, typename ... Args,
        std::enable_if_t<std::is_constructible<lite_ptr<T, default_deleter<T>, alignof(T), std::decay_t<Alloc>>,
                                               in_place_t, std::allocator_arg_t, Alloc&&, Args&&...>::value>* = nullptr>
    auto make_lite_ptr_with_allocator(Alloc&& alloc, Args&&... args)
        noexcept(std::is_nothrow_constructible<lite_ptr<T, default_deleter<T>, alignof(T), std::decay_t<Alloc>>,
            in_place_t, std::allocator_arg_t, Alloc&&, Args&&...>::value)
            -> lite_ptr<T, default_deleter<T>, alignof(T), std::decay_t<Alloc>> {
        using ptr_t = lite_ptr<T, default_deleter<T>, alignof(T), std::decay_t<Alloc>>;
        return ptr_t(in_place, std::allocator_arg, std::forward<Alloc>(alloc), std::forward<Args>(args)...);
    }

    // Comparison operators
    template <typename T, typename F, size_t align, typename A>
    bool operator==(const lite_ptr<T, F, align, A>& lhs, const lite_ptr<T, F, align, A>& rhs) noexcept {
        return lhs.get() == rhs.get();
    }

    template <typename T, typename F, size_t align, typename A>
    bool operator!=(const lite_ptr<T, F, align, A>& lhs, const lite_ptr<T, F, align, A>& rhs) noexcept {
        return !(lhs == rhs);
    }

    template <typename T, typename F, size_t align, typename A>
    bool operator<(const lite_ptr<T, F, align, A>& lhs, const lite_ptr<T, F, align, A>& rhs) noexcept {
        return lhs.get() < rhs.get();
    }

    template <typename T, typename F, size_t align, typename A>
    bool operator==(const lite_ptr<T, F, align, A>& ptr, std::nullptr_t) noexcept {
        return !ptr;
    }

    template <typename T, typename F, size_t align, typename A>
    bool operator==(std::nullptr_t, const lite_ptr<T, F, align, A>& ptr) noexcept {
        return !ptr;
    }

    template <typename T, typename F, size_t align, typename A>
    bool operator!=(const lite_ptr<T, F, align, A>& ptr, std::nullptr_t) noexcept {
        return static_cast<bool>(ptr);
    }

    template <typename T, typename F, size_t align, typename A>
    bool operator!=(std::nullptr_t, const lite_ptr<T, F, align, A>& ptr) noexcept {
        return static_cast<bool>(ptr);
    }

    template <typename R>
    struct is_lite_ptr_impl : std::false_type { };

    template <typename T, typename F, size_t align, typename A>
    struct is_lite_ptr_impl<lite_ptr<T, F, align, A>> : std::true_type { };

    template <typename R>
    struct is_lite_ptr : is_lite_ptr_impl<std::decay_t<R>> { };

    template <typename R>
    constexpr bool is_lite_ptr_v = is_lite_ptr<R>::value;
}

#endif //LF_LITE_PTR
