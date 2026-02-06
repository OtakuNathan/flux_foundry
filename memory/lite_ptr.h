//
// Created by Nathan on 08/01/2026.
//

#ifndef LF_TEST_REF_COUNT_H
#define LF_TEST_REF_COUNT_H

#include <atomic>
#include <stdexcept>
#include <cassert>
#include <new>

#include "../base/inplace_base.h"
#include "flat_storage.h"
#include "aligned_alloc.h"

namespace lite_fnds {

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

    template <typename T, typename F = default_deleter<T>, size_t align = alignof(T)>
    class lite_ptr {
        static_assert(align >= alignof(T), "align must be >= alignof(T)");
        static_assert((align & (align - 1)) == 0, "align must be power of two");;

        using op = raw_inplace_storage_base<T>;

        struct cb_t {
            alignas(align) unsigned char data[sizeof(T)];
            compressed_pair<std::atomic<size_t>, F> cb;

            template <typename G, typename ... Args>
            cb_t(G&& g, Args&& ... args)
                noexcept(conjunction_v<std::is_nothrow_constructible<T, Args&&...>,
                        std::is_nothrow_constructible<F, G&&>>)
                : cb(1, std::forward<G>(g)) {
                op::construct_at(data, std::forward<Args>(args)...);
            }

            ~cb_t() noexcept {
                cb.second()(reinterpret_cast<T*>(data));
            }
        };

        struct guard {
            cb_t* cb;
            explicit guard(cb_t* cb_) : cb(cb_) {}
            ~guard() noexcept {
                aligned_free(cb);
            }
        };

        cb_t* cb;
    public:
        using element_type = T;

        lite_ptr() noexcept : cb(nullptr) {}

        template <typename ... Args, typename F_ = F,
                std::enable_if_t<conjunction_v<std::is_same<F_, default_deleter<T>>,
#if LFNDS_HAS_EXCEPTIONS
                        std::is_constructible<T, Args&&...>
#else
                        std::is_nothrow_constructible<T, Args&&...>
#endif
        >>* = nullptr>
        explicit lite_ptr(in_place_t, Args&& ... args)
#if !LFNDS_COMPILER_HAS_EXCEPTIONS
            noexcept(conjunction_v<std::is_nothrow_constructible<cb_t, default_deleter<T>&&, Args&&...>>)
#endif
            : cb {nullptr} {
            auto cb_ = static_cast<cb_t*>(aligned_alloc(alignof(cb_t), (sizeof(cb_t) + alignof(cb_t) - 1) & ~(alignof(cb_t) - 1)));
            if (!cb_) {
#if LFNDS_COMPILER_HAS_EXCEPTIONS
                throw std::bad_alloc();
#else
                return;
#endif
            }

            guard g(cb_);
            new (g.cb) cb_t(default_deleter<T>(), std::forward<Args>(args)...);
            cb = g.cb;
            g.cb = nullptr;
        }

        template <typename G, typename ... Args, typename F_ = F,
                std::enable_if_t<conjunction_v<
                        negation<std::is_same<F_, default_deleter<T>>>,
#if LFNDS_HAS_EXCEPTIONS
                        std::is_constructible<F_, G&&>, std::is_constructible<T, Args&&...>
#else
                        std::is_nothrow_constructible<F_, G&&>, std::is_nothrow_constructible<T, Args&&...>
#endif
        >>* = nullptr>
        lite_ptr(in_place_t, G&& g, Args&& ... args)
#if !LFNDS_COMPILER_HAS_EXCEPTIONS
            noexcept(conjunction_v<std::is_nothrow_constructible<cb_t, G&&, Args&&...>>)
#endif
            : cb {nullptr} {
            auto cb_ = static_cast<cb_t*>(aligned_alloc(alignof(cb_t), (sizeof(cb_t) + alignof(cb_t) - 1) & ~(alignof(cb_t) - 1)));
            if (!cb_) {
#if LFNDS_COMPILER_HAS_EXCEPTIONS
                throw std::bad_alloc();
#else
                return;
#endif
            }
            guard g_(cb_);
            new (g_.cb) cb_t(std::forward<G>(g), std::forward<Args>(args)...);
            cb = g_.cb;
            g_.cb = nullptr;
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
            UNLIKELY_IF(ccb && ccb->cb.first().fetch_sub(1, std::memory_order_release) == 1) {
                std::atomic_thread_fence(std::memory_order_acquire);
                ccb->~cb_t();
                aligned_free(ccb);
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

    template <typename T, typename F, size_t align>
    void swap(lite_ptr<T, F, align>& lhs , lite_ptr<T, F, align>& rhs) noexcept {
        lhs.swap(rhs);
    }

    template <typename T, typename ... Args,
            std::enable_if_t<std::is_constructible<lite_ptr<T>, in_place_t, Args&&...>::value>* = nullptr>
    lite_ptr<T> make_lite_ptr(Args&&... args)
        noexcept(std::is_nothrow_constructible<lite_ptr<T>, in_place_t, Args&&...>::value) {
        return lite_ptr<T>(in_place, std::forward<Args>(args)...);
    }

    // Comparison operators
    template <typename T, typename F, size_t align>
    bool operator==(const lite_ptr<T, F, align>& lhs, const lite_ptr<T, F, align>& rhs) noexcept {
        return lhs.get() == rhs.get();
    }

    template <typename T, typename F, size_t align>
    bool operator!=(const lite_ptr<T, F, align>& lhs, const lite_ptr<T, F, align>& rhs) noexcept {
        return !(lhs == rhs);
    }

    template <typename T, typename F, size_t align>
    bool operator<(const lite_ptr<T, F, align>& lhs, const lite_ptr<T, F, align>& rhs) noexcept {
        return lhs.get() < rhs.get();
    }

    template <typename T, typename F, size_t align>
    bool operator==(const lite_ptr<T, F, align>& ptr, std::nullptr_t) noexcept {
        return !ptr;
    }

    template <typename T, typename F, size_t align>
    bool operator==(std::nullptr_t, const lite_ptr<T, F, align>& ptr) noexcept {
        return !ptr;
    }

    template <typename T, typename F, size_t align>
    bool operator!=(const lite_ptr<T, F, align>& ptr, std::nullptr_t) noexcept {
        return static_cast<bool>(ptr);
    }

    template <typename T, typename F, size_t align>
    bool operator!=(std::nullptr_t, const lite_ptr<T, F, align>& ptr) noexcept {
        return static_cast<bool>(ptr);
    }

    template <typename R>
    struct is_lite_ptr_impl : std::false_type { };

    template <typename T>
    struct is_lite_ptr_impl<lite_ptr<T>> : std::true_type { };

    template <typename R>
    struct is_lite_ptr : is_lite_ptr_impl<std::decay_t<R>> { };

    template <typename R>
    constexpr bool is_lite_ptr_v = is_lite_ptr<R>::value;
}

#endif //LF_LITE_PTR
