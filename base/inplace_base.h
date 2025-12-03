#ifndef LITE_FNDS_INPLACE_BASE_H
#define LITE_FNDS_INPLACE_BASE_H

#include <type_traits>
#include <cstddef>
#include <new>
#include <utility>
#include "traits.h"

namespace lite_fnds {
    // ctor deleter
    template <typename T, bool EnableCopy, bool EnableMove>
    struct ctor_delete_base;

    template <typename T>
    struct ctor_delete_base <T, true, true> {
        ctor_delete_base() = default;
        ctor_delete_base(const ctor_delete_base&) = default;
        ctor_delete_base(ctor_delete_base&&) noexcept = default;
        ctor_delete_base& operator=(const ctor_delete_base&) = default;
        ctor_delete_base& operator=(ctor_delete_base&&) noexcept = default;
    };

    template <typename T>
    struct ctor_delete_base<T, true, false> {
        ctor_delete_base() = default;
        ctor_delete_base(const ctor_delete_base&) = default;
        ctor_delete_base(ctor_delete_base&&) = delete;
        ctor_delete_base& operator=(const ctor_delete_base&) = default;
        ctor_delete_base& operator=(ctor_delete_base&&) noexcept = default;
    };

    template <typename T>
    struct ctor_delete_base<T, false, true> {
        ctor_delete_base() = default;
        ctor_delete_base(const ctor_delete_base&) = delete;
        ctor_delete_base(ctor_delete_base&&) noexcept = default;
        ctor_delete_base& operator=(const ctor_delete_base&) = default;
        ctor_delete_base& operator=(ctor_delete_base&&) noexcept = default;
    };

    template <typename T>
    struct ctor_delete_base<T, false, false> {
        ctor_delete_base() = default;
        ctor_delete_base(const ctor_delete_base&) = delete;
        ctor_delete_base(ctor_delete_base&&) noexcept = delete;
        ctor_delete_base& operator=(const ctor_delete_base&) = default;
        ctor_delete_base& operator=(ctor_delete_base&&) noexcept = default;
    };

    // assign deleter
    template <typename T, bool EnableCopy, bool EnableMove>
    struct assign_delete_base;

    template  <typename T>
    struct assign_delete_base <T, true, true> {
        assign_delete_base() = default;
        assign_delete_base(const assign_delete_base&) = default;
        assign_delete_base(assign_delete_base&&) noexcept = default;
        assign_delete_base& operator=(const assign_delete_base&) = default;
        assign_delete_base& operator=(assign_delete_base&&) noexcept = default;
    };

    template <typename T>
    struct assign_delete_base<T, true, false> {
        assign_delete_base() = default;
        assign_delete_base(const assign_delete_base&) = default;
        assign_delete_base(assign_delete_base&&) noexcept = default;
        assign_delete_base& operator=(const assign_delete_base&) = default;
        assign_delete_base& operator=(assign_delete_base&&) noexcept = delete;
    };

    template <typename T>
    struct assign_delete_base<T, false, true> {
        assign_delete_base() = default;
        assign_delete_base(const assign_delete_base&) = default;
        assign_delete_base(assign_delete_base&&) noexcept = default;
        assign_delete_base& operator=(const assign_delete_base&) = delete;
        assign_delete_base& operator=(assign_delete_base&&) noexcept = default;
    };

    template <typename T>
    struct assign_delete_base<T, false, false> {
        assign_delete_base() = default;
        assign_delete_base(const assign_delete_base&) = default;
        assign_delete_base(assign_delete_base&&) noexcept = default;
        assign_delete_base& operator=(const assign_delete_base&) = delete;
        assign_delete_base& operator=(assign_delete_base&&) = delete;
    };

    template <typename T>
    struct raw_inplace_storage_operations {
#if !LFNDS_HAS_EXCEPTIONS
        static_assert(std::is_nothrow_destructible<T>::value, "T must be nothrow destructible");
#endif
    private:
        struct guard {
            T* _p{};
            ~guard() noexcept(std::is_nothrow_destructible<T>::value) { if (_p) { _p->~T(); } }
            T* get() const noexcept { return _p; }
        };

        static T* ptr(void* _data) noexcept {
            return static_cast<T*>(_data);
        }

        static const T* ptr(const void* _data) noexcept {
            return static_cast<const T*>(_data);
        }

        // this must be called when no value have been created yet.
        template <typename ... Args, std::enable_if_t<std::is_constructible<T, Args&&...>::value>* = nullptr>
        static T* _construct_at(void* addr, Args&& ... args)
            noexcept (std::is_nothrow_constructible<T, Args&&...>::value) {
#if !LFNDS_HAS_EXCEPTIONS
            static_assert(std::is_nothrow_constructible<T, Args&&...>::value,
                "T must be nothrow constructible with Args...");
#endif
            return ::new (addr) T(std::forward<Args>(args)...);
        }

        template <typename ... Args, std::enable_if_t<conjunction_v<
            negation<std::is_constructible<T, Args&&...>>, is_aggregate_constructible<T, Args&&...>>>* = nullptr>
        static T* _construct_at(void* addr, Args&& ... args)
            noexcept (is_nothrow_aggregate_constructible<T, Args&&...>::value) {
#if !LFNDS_HAS_EXCEPTIONS
            static_assert(is_nothrow_aggregate_constructible<T, Args&&...>::value,
                "T must be nothrow aggregate constructible with Args...");
#endif
            return ::new (addr) T{ std::forward<Args>(args)... };
        }

    public:
        using value_type = T;

        static void destroy_at(void* addr)
            noexcept(std::is_nothrow_destructible<T>::value) {
            ptr(addr)->~T();
        }

        template <typename ... Args, std::enable_if_t<disjunction_v<
            std::is_constructible<T, Args&&...>, is_aggregate_constructible<T, Args&&...>>>* = nullptr>
        static T* construct_at(void* addr, Args&& ... args)
            noexcept (std::conditional_t<std::is_constructible<T, Args&&...>::value,
                    std::is_nothrow_constructible<T, Args&&...>,
                    is_nothrow_aggregate_constructible<T, Args&&...>>::value) {
            return _construct_at(addr, std::forward<Args>(args)...);
        }

        template <typename U, typename ... Args,
            std::enable_if_t<std::is_constructible<T, std::initializer_list<U>, Args &&...>::value>* = nullptr>
        static T* construct_at(void* addr, std::initializer_list<U> il, Args&& ... args)
            noexcept (std::is_nothrow_constructible<T, std::initializer_list<U>, Args &&...>::value) {
            return  _construct_at(addr, il, std::forward<Args>(args)...);
        }

#if LFNDS_HAS_EXCEPTIONS
        template <typename ... Args, std::enable_if_t<conjunction_v<
            std::is_constructible<T, Args&&...>,
            std::is_nothrow_move_assignable<T>>>* = nullptr>
        static void emplace_at(void* addr, Args &&... args)
            noexcept(std::is_nothrow_constructible<T, Args&&...>::value) {
            alignas(alignof(T)) unsigned char _buffer[sizeof(T)];
            guard g{ construct_at(_buffer, std::forward<Args>(args)...) };
            *ptr(addr) = std::move(*g.get());
        }

        template <typename ... Args, std::enable_if_t<conjunction_v<
            std::is_constructible<T, Args&&...>,
            negation<std::is_nothrow_move_assignable<T>>,
            std::is_nothrow_copy_assignable<T>>>* = nullptr>
        static void emplace_at(void* addr, Args &&... args)
            noexcept(std::is_nothrow_constructible<T, Args&&...>::value) {
            alignas(alignof(T)) unsigned char _buffer[sizeof(T)];
            guard g{ construct_at(_buffer, std::forward<Args>(args)...) };
            *ptr(addr) = *g.get();
        }

        template <typename ... Args, std::enable_if_t<conjunction_v<
            std::is_constructible<T, Args&&...>,
            negation<std::is_nothrow_move_assignable<T>>,
            negation<std::is_nothrow_copy_assignable<T>>,
            std::is_nothrow_move_constructible<T>>>* = nullptr>
        static void emplace_at(void* addr, Args &&... args)
            noexcept(std::is_nothrow_destructible<T>::value&& std::is_nothrow_constructible<T, Args&&...>::value) {
            alignas(alignof(T)) unsigned char _buffer[sizeof(T)];
            guard g{ construct_at(_buffer, std::forward<Args>(args)...) };
            destroy_at(addr);
            construct_at(addr, std::move(*g.get()));
        }

        template <typename ... Args, std::enable_if_t<conjunction_v<
            std::is_constructible<T, Args&&...>,
            negation<std::is_nothrow_move_assignable<T>>,
            negation<std::is_nothrow_copy_assignable<T>>,
            negation<std::is_nothrow_move_constructible<T>>,
            std::is_nothrow_copy_constructible<T>>>* = nullptr>
        static void emplace_at(void* addr, Args &&... args)
            noexcept(std::is_nothrow_destructible<T>::value&& std::is_nothrow_constructible<T, Args&&...>::value) {
            alignas(alignof(T)) unsigned char _buffer[sizeof(T)];
            guard g{ construct_at(_buffer, std::forward<Args>(args)...) };
            destroy_at(addr);
            construct_at(addr, *g.get());
        }

        template <typename ... Args, std::enable_if_t<conjunction_v<
            std::is_constructible<T, Args &&...>,
            negation<std::is_nothrow_move_assignable<T> >,
            negation<std::is_nothrow_copy_assignable<T> >,
            negation<std::is_nothrow_move_constructible<T> >,
            negation<std::is_nothrow_copy_constructible<T>>,
            std::is_nothrow_constructible<T, Args&&...>>>* = nullptr>
        static void emplace_at(void *addr, Args &&... args)
            noexcept(std::is_nothrow_destructible<T>::value && std::is_nothrow_constructible<T, Args &&...>::value) {
            destroy_at(addr);
            construct_at(addr, std::forward<Args>(args)...);
        }
#else
        template <typename ... Args, std::enable_if_t<std::is_nothrow_constructible<T, Args&&...>::value>* = nullptr>
        static void emplace_at(void *addr, Args &&... args) noexcept {
            destroy_at(addr);
            construct_at(addr, std::forward<Args>(args)...);
        }
#endif
    };


    template <typename T, size_t len = sizeof(T), size_t align = alignof(T)>
    struct raw_inplace_storage_base : raw_inplace_storage_operations<T> {
        static_assert (sizeof(T) <= len, "the length provided is not sufficient for storing object");
        static_assert((align & (align - 1)) == 0, "align must be a power-of-two");
        static_assert(align >= alignof(T), "align must be >= alignof(T)");
        static_assert(align % alignof(T) == 0, "align must be compatible with alignof(T)");
    private:
        using base = raw_inplace_storage_operations<T>;
    public:
        alignas(align) unsigned char _data[len];

        T* ptr() noexcept {
            return reinterpret_cast<T*>(_data);
        }

        const T* ptr() const noexcept {
            return reinterpret_cast<const T*>(_data);
        }

        constexpr raw_inplace_storage_base() = default;
        ~raw_inplace_storage_base() = default;

        template <typename ... Args, std::enable_if_t<disjunction_v<
                std::is_constructible<T, Args&&...>, is_aggregate_constructible<T, Args&&...>>>* = nullptr>
        explicit raw_inplace_storage_base(Args&& ... args)
            noexcept (std::is_nothrow_constructible<T, Args&&...>::value) {
            this->construct(std::forward<Args>(args)...);
        }

        template <typename U, typename... Args,
            std::enable_if_t<std::is_constructible<T, std::initializer_list<U>, Args &&...>::value>* = nullptr>
        explicit raw_inplace_storage_base(std::initializer_list<U> il, Args&&... args)
            noexcept (std::is_nothrow_constructible<T, std::initializer_list<U>, Args&&...>::value) {
            this->construct(il, std::forward<Args>(args)...);
        }

        void destroy() noexcept(std::is_nothrow_destructible<T>::value) {
            base::destroy_at(_data);
        }

        // this must be called when no value have been created yet.
        template <typename ... Args, std::enable_if_t<disjunction_v<
            std::is_constructible<T, Args&&...>, is_aggregate_constructible<T, Args&&...>>>* = nullptr >
        void construct(Args&& ... args)
            noexcept (std::conditional_t<std::is_constructible<T, Args&&...>::value,
                std::is_nothrow_constructible<T, Args&&...>,
                is_nothrow_aggregate_constructible<T, Args&&...>>::value) {
            base::construct_at(_data, std::forward<Args>(args)...);
        }

        template <typename U, typename... Args,
            std::enable_if_t<std::is_constructible<T, std::initializer_list<U>, Args&&...>::value>* = nullptr >
        void construct(std::initializer_list<U> il, Args&& ... args)
        noexcept (std::is_nothrow_constructible<T, std::initializer_list<U>, Args&&...>::value) {
            base::construct_at(_data, il, std::forward<Args>(args)...);
        }

        template <typename ... Args>
        void emplace(Args &&... args)
            noexcept(noexcept(base::emplace_at(_data, std::forward<Args>(args)...))) {
            base::emplace_at(_data, std::forward<Args>(args)...);
        }
    };
}

#endif
