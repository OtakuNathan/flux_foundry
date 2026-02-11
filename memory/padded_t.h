//
// Created by Nathan on 08/01/2026.
//

#ifndef FLUX_FOUNDRY_PADDED_T_H
#define FLUX_FOUNDRY_PADDED_T_H

#include <initializer_list>
#include "../base/type_utility.h"

namespace flux_foundry {
    template <typename T, size_t align = alignof(T)>
    struct alignas(align) TS_EMPTY_BASES padded_t :
#if FLUEX_FOUNDRY_HAS_EXCEPTIONS
            ctor_delete_base<T, std::is_copy_constructible<T>::value, std::is_move_constructible<T>::value>,
            assign_delete_base<T, std::is_copy_assignable<T>::value, std::is_move_assignable<T>::value>
#else
        flux_foundry::ctor_delete_base<T, std::is_nothrow_copy_constructible<T>::value, std::is_nothrow_move_constructible<T>::value>,
            flux_foundry::assign_delete_base<T, std::is_nothrow_copy_assignable<T>::value, std::is_nothrow_move_assignable<T>::value>
#endif
    {
        static_assert(!std::is_void<T>::value, "T must not be void");
        static_assert(align > 0, "pad_to must be > 0");
        static_assert((align & (align - 1)) == 0, "align must be power of two");
        static_assert(align >= alignof(T), "align must be >= alignof(T)");
        T val_;
    public:
        padded_t() = default;

        template <typename... Args,
                std::enable_if_t<conjunction_v<negation<is_self_constructing<padded_t, Args &&...>>,
#if FLUEX_FOUNDRY_HAS_EXCEPTIONS
                        std::is_constructible<T, Args &&...>
#else
                        std::is_nothrow_constructible<T, Args &&...>
#endif
                >> * = nullptr>
        constexpr explicit padded_t(Args &&... args)
        noexcept(std::is_nothrow_constructible<T, Args && ...>::value)
                : val_(std::forward<Args>(args)...) {
        }

        template <typename U, typename ... Args,
                std::enable_if_t<
#if FLUEX_FOUNDRY_HAS_EXCEPTIONS
                        std::is_constructible<T, std::initializer_list<U>, Args &&...>::value
#else
                        std::is_nothrow_constructible<T, std::initializer_list<U>, Args &&...>::value
#endif
                >* = nullptr>
        constexpr padded_t(std::initializer_list<U> il, Args &&... args)
        noexcept(std::is_nothrow_constructible<T, std::initializer_list<U>, Args &&...>::value)
                : val_(il, std::forward<Args>(args)...) {
        }

        operator T&() & noexcept { return val_; }
        operator const T&() const & noexcept {  return val_; }
        operator T&&() && noexcept { return std::move(val_); }

        T& get() & noexcept { return val_; }
        const T& get() const & noexcept { return val_; }
        T&& get() && noexcept { return std::move(val_); }

        T* address_of() & { return &val_; }
        const T* address_of() const & { return &val_; }
    };

    template <typename T, size_t align>
    constexpr bool operator==(const padded_t<T, align> &lhs, const padded_t<T, align> &rhs)
    noexcept(noexcept(lhs.get() == rhs.get())) {
        return lhs.get() == rhs.get();
    }

    template <typename T, size_t align>
    constexpr bool operator!=(const padded_t<T, align> &lhs, const padded_t<T, align> &rhs)
    noexcept(noexcept(lhs.get() == rhs.get())) {
        return !(lhs.get() == rhs.get());
    }

    template <typename T, size_t align>
    constexpr bool operator<(const padded_t<T, align> &lhs, const padded_t<T, align> &rhs)
    noexcept(noexcept(lhs.get() < rhs.get())) {
        return lhs.get() < rhs.get();
    }

    template <typename T, size_t align>
    constexpr bool operator<=(const padded_t<T, align> &lhs, const padded_t<T, align> &rhs)
    noexcept(noexcept(lhs.get() <= rhs.get())) {
        return lhs.get() <= rhs.get();
    }

    template <typename T, size_t align>
    constexpr bool operator>(const padded_t<T, align> &lhs, const padded_t<T, align> &rhs)
    noexcept(noexcept(lhs.get() <= rhs.get())) {
        return !(lhs.get() <= rhs.get());
    }

    template <typename T, size_t align>
    constexpr bool operator>=(const padded_t<T, align> &lhs, const padded_t<T, align> &rhs)
    noexcept(noexcept(lhs.get() < rhs.get())) {
        return !(lhs.get() < rhs.get());
    }
}

#endif
