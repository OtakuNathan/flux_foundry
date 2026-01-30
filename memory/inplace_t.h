//
// Created by Nathan on 01/09/2025.
//

#ifndef LITE_FNDS_INPLACE_T_H
#define LITE_FNDS_INPLACE_T_H

#include "../base/inplace_base.h"

namespace lite_fnds {
    template <typename T, size_t len = sizeof(T), size_t align = alignof(T),
        bool = std::is_trivially_destructible<T>::value >
    struct inplace_storage_base;

    template <typename T, size_t len, size_t align>
    struct TS_EMPTY_BASES inplace_storage_base<T, len, align, true> : private raw_inplace_storage_base<T, len, align> {
        static_assert(sizeof(T) <= len, "the length provided is not sufficient for storing object");
        static_assert((align & (align - 1)) == 0, "align must be a power-of-two");
        static_assert(align >= alignof(T), "align must be >= alignof(T)");

    private:
        using base = raw_inplace_storage_base<T, len, align>;
        bool _has_value;

    public:
        constexpr inplace_storage_base() noexcept :
            _has_value{false} {
        }

        ~inplace_storage_base() noexcept = default;

        template <typename ... Args,
            typename = std::enable_if_t<conjunction_v<
                    negation<is_self_constructing<inplace_storage_base, Args&&...>>,
                    disjunction<
#if LFNDS_HAS_EXCEPTIONS
                std::is_constructible<T, Args &&...>, is_aggregate_constructible<T, Args &&...>
#else
                std::is_nothrow_constructible<T, Args &&...>, is_nothrow_aggregate_constructible<T, Args &&...>
#endif
        >>>>
        explicit inplace_storage_base(Args &&... args)
            noexcept(noexcept(static_cast<base*>(nullptr)->construct(std::declval<Args&&>()...))) :
            _has_value{false} {
            this->construct(std::forward<Args>(args)...);
        }

        template <typename U, size_t _len, size_t _align,
            std::enable_if_t<conjunction_v<negation<std::is_same<T, U> >,
#if LFNDS_HAS_EXCEPTIONS
            std::is_constructible<T, const U &>
#else
            std::is_nothrow_constructible<T, const U &>
#endif
        > >* =
                    nullptr>
        explicit inplace_storage_base(const inplace_storage_base<U, _len, _align> &rhs)
            noexcept(std::is_nothrow_constructible<T, const U &>::value) :
            _has_value{false} {
            if (rhs.has_value()) {
                this->construct(rhs.get());
            }
        }

        template <typename U, size_t _len, size_t _align,
            std::enable_if_t<conjunction_v<negation<std::is_same<T, U> >,
#if LFNDS_HAS_EXCEPTIONS
            std::is_constructible<T, U &&>
#else
            std::is_nothrow_constructible<T, U &&>
#endif
            > >* = nullptr>
        explicit inplace_storage_base(inplace_storage_base<U, _len, _align> &&rhs)
            noexcept(std::is_nothrow_constructible<T, U &&>::value) :
            _has_value{false} {
            if (rhs.has_value()) {
                this->construct(std::move(rhs).get());
            }
        }

        template <typename U, size_t _len, size_t _align,
            std::enable_if_t<conjunction_v<negation<std::is_same<T, U> >,
#if LFNDS_HAS_EXCEPTIONS
            std::is_constructible<T, const U &>
#else
                std::is_nothrow_constructible<T, const U &>
#endif
        > >* =
                    nullptr>
        inplace_storage_base &operator=(const inplace_storage_base<U, _len, _align> &rhs)
            noexcept(noexcept(std::declval<inplace_storage_base&>().emplace(std::declval<T>()))) {
            if (!rhs.has_value()) {
                this->destroy();
            } else {
                T tmp(rhs.get());
                this->emplace(std::move(tmp));
            }
            return *this;
        }

        template <typename U, size_t _len, size_t _align,
            std::enable_if_t<conjunction_v<negation<std::is_same<T, U> >,
#if LFNDS_HAS_EXCEPTIONS
            std::is_constructible<T, U &&>
#else
            std::is_nothrow_constructible<T, U &&>
#endif
            > >* = nullptr>
        inplace_storage_base &operator=(inplace_storage_base<U, _len, _align> &&rhs)
            noexcept(noexcept(std::declval<inplace_storage_base&>().emplace(std::declval<T>()))) {
            if (!rhs.has_value()) {
                this->destroy();
            } else {
                T tmp(std::move(rhs.get()));
                this->emplace(std::move(tmp));
            }
            return *this;
        }

        // this must be called when no value have been created yet.
        template <typename... Args, typename = std::enable_if_t<
            disjunction_v<
#if LFNDS_HAS_EXCEPTIONS
                std::is_constructible<T, Args &&...>, is_aggregate_constructible<T, Args &&...>
#else
                std::is_nothrow_constructible<T, Args &&...>, is_nothrow_aggregate_constructible<T, Args &&...>
#endif
        > > >
        void construct(Args &&... args)
                noexcept(noexcept(static_cast<base*>(nullptr)->construct(std::declval<Args&&>()...))) {
            assert(!_has_value && "construct() requires no live object");
            static_cast<base &>(*this).construct(std::forward<Args>(args)...);
            _has_value = true;
        }

        template <typename... Args, std::enable_if_t<conjunction_v<
            std::is_constructible<T, Args &&...>, can_strong_replace<T> >, int>  = 0>
        void emplace(Args &&... args)
            noexcept(noexcept(static_cast<base*>(nullptr)->emplace(std::declval<Args&&>()...))) {
            if (!this->has_value()) {
                this->construct(std::forward<Args>(args)...);
                return;
            }
            static_cast<base &>(*this).emplace(std::forward<Args>(args)...);
        }

        template <typename... Args, std::enable_if_t<conjunction_v<
            std::is_constructible<T, Args &&...>, negation<can_strong_replace<T> > >, int>  = 0>
        void emplace(Args &&... args)
            noexcept(std::is_nothrow_destructible<T>::value && std::is_nothrow_constructible<T, Args &&...>::value) {
            if (!this->has_value()) {
                this->construct(std::forward<Args>(args)...);
                return;
            }
            this->destroy();
            this->construct(std::forward<Args>(args)...);
        }

        constexpr bool has_value() const noexcept {
            return _has_value;
        }

        void destroy() noexcept(noexcept(static_cast<base*>(nullptr)->destroy())) {
            if (has_value()) {
                static_cast<base &>(*this).destroy();
                _has_value = false;
            }
        }

        constexpr T &get() & noexcept {
            assert(this->has_value() && "attempting to access non-created value");
            return *static_cast<base &>(*this).ptr();
        }

        constexpr const T &get() const & noexcept {
            assert(this->has_value() && "attempting to access non-created value");
            return *static_cast<const base &>(*this).ptr();
        }

        constexpr T &&get() && noexcept {
            assert(this->has_value() && "attempting to access non-created value");
            return std::move(*static_cast<base &&>(*this).ptr());
        }
    };

    template <typename T, size_t len, size_t align>
    struct TS_EMPTY_BASES inplace_storage_base<T, len, align, false> : private raw_inplace_storage_base<T, len, align> {
        static_assert(sizeof(T) <= len, "the length provided is not sufficient for storing object");
        static_assert((align & (align - 1)) == 0, "align must be a power-of-two");
        static_assert(align >= alignof(T), "align must be >= alignof(T)");

    private:
        using base = raw_inplace_storage_base<T, len, align>;
        bool _has_value;

    public:
        constexpr inplace_storage_base() noexcept
            : _has_value{false} {
        }

        ~inplace_storage_base() {
            destroy();
        }

        template <typename ... Args,
                typename = std::enable_if_t<conjunction_v<
                    negation<is_self_constructing<inplace_storage_base, Args&&...>>,
                    disjunction<
#if LFNDS_HAS_EXCEPTIONS
                        std::is_constructible<T, Args &&...>, is_aggregate_constructible<T, Args &&...>
#else
                        std::is_nothrow_constructible<T, Args &&...>, is_nothrow_aggregate_constructible<T, Args &&...>
#endif
        >>>>
        explicit inplace_storage_base(Args &&... args)
            noexcept(noexcept(static_cast<base*>(nullptr)->construct(std::declval<Args&&>()...))) :
            _has_value{false} {
            this->construct(std::forward<Args>(args)...);
        }

        template <typename U, size_t _len, size_t _align,
            std::enable_if_t<conjunction_v<negation<std::is_same<T, U> >,
#if LFNDS_HAS_EXCEPTIONS
                std::is_constructible<T, const U &>
#else
                std::is_nothrow_constructible<T, const U &>
#endif
            > >* =nullptr>
        explicit inplace_storage_base(const inplace_storage_base<U, _len, _align> &rhs)
            noexcept(std::is_nothrow_constructible<T, const U &>::value) :
            _has_value{false} {
            if (rhs.has_value()) {
                this->construct(rhs.get());
            }
        }

        template <typename U, size_t _len, size_t _align,
            std::enable_if_t<conjunction_v<negation<std::is_same<T, U> >,
#if LFNDS_HAS_EXCEPTIONS
                std::is_constructible<T, U &&>
#else
                std::is_nothrow_constructible<T, U &&>
#endif
            > >* = nullptr>
        explicit inplace_storage_base(inplace_storage_base<U, _len, _align> &&rhs)
            noexcept(std::is_nothrow_constructible<T, U &&>::value) :
            _has_value{false} {
            if (rhs.has_value()) {
                this->construct(std::move(rhs).get());
            }
        }

        template <typename U, size_t _len, size_t _align,
            std::enable_if_t<conjunction_v<negation<std::is_same<T, U> >,
#if LFNDS_HAS_EXCEPTIONS
                std::is_constructible<T, const U &>
#else
                std::is_nothrow_constructible<T, const U &>
#endif
            > >* =
                    nullptr>
        inplace_storage_base &operator=(const inplace_storage_base<U, _len, _align> &rhs)
            noexcept(std::is_nothrow_copy_constructible<T>::value
                && noexcept(std::declval<inplace_storage_base&>().emplace(std::declval<T&&>()))) {
            if (!rhs.has_value()) {
                this->destroy();
            } else {
                T tmp(rhs.get());
                this->emplace(std::move(tmp));
            }
            return *this;
        }

        template <typename U, size_t _len, size_t _align,
            std::enable_if_t<conjunction_v<negation<std::is_same<T, U> >, std::is_constructible<T, U &&> > >* = nullptr>
        inplace_storage_base &operator=(inplace_storage_base<U, _len, _align> &&rhs)
            noexcept(std::is_nothrow_move_constructible<T>::value
                && noexcept(std::declval<inplace_storage_base&>().emplace(std::declval<T&&>()))) {
            if (!rhs.has_value()) {
                this->destroy();
            } else {
                T tmp(std::move(rhs.get()));
                this->emplace(std::move(tmp));
            }
            return *this;
        }

        // this must be called when no value have been created yet.
        template <typename... Args, typename = std::enable_if_t<
            disjunction_v<
#if LFNDS_HAS_EXCEPTIONS
                std::is_constructible<T, Args &&...>, is_aggregate_constructible<T, Args &&...>
#else
                std::is_nothrow_constructible<T, Args &&...>, is_nothrow_aggregate_constructible<T, Args &&...>
#endif
            > > >
        void construct(Args &&... args)
            noexcept(noexcept(static_cast<base*>(nullptr)->construct(std::declval<Args&&>()...))) {
            assert(!_has_value && "construct() requires no live object");
            static_cast<base &>(*this).construct(std::forward<Args>(args)...);
            _has_value = true;
        }

        template <typename... Args, std::enable_if_t<conjunction_v<
            std::is_constructible<T, Args &&...>, can_strong_replace<T> >, int>  = 0>
        void emplace(Args &&... args)
            noexcept(noexcept(static_cast<base*>(nullptr)->emplace(std::declval<Args&&>()...))) {
            if (!this->has_value()) {
                this->construct(std::forward<Args>(args)...);
                return;
            }
            static_cast<base &>(*this).emplace(std::forward<Args>(args)...);
        }

        template <typename... Args, std::enable_if_t<conjunction_v<
            std::is_constructible<T, Args &&...>, negation<can_strong_replace<T> > >, int>  = 0>
        void emplace(Args &&... args)
            noexcept(std::is_nothrow_destructible<T>::value && std::is_nothrow_constructible<T, Args &&...>::value) {
            if (!this->has_value()) {
                this->construct(std::forward<Args>(args)...);
                return;
            }
            this->destroy();
            this->construct(std::forward<Args>(args)...);
        }

        constexpr bool has_value() const noexcept {
            return _has_value;
        }

        void destroy() noexcept(noexcept(static_cast<base*>(nullptr)->destroy())) {
            if (has_value()) {
                static_cast<base &>(*this).destroy();
                _has_value = false;
            }
        }

        constexpr T &get() & noexcept {
            assert(this->has_value() && "attempting to access non-created value");
            return *static_cast<base &>(*this).ptr();
        }

        constexpr const T &get() const & noexcept {
            assert(this->has_value() && "attempting to access non-created value");
            return *static_cast<base &>(*this).ptr();
        }

        constexpr T &&get() && noexcept {
            assert(this->has_value() && "attempting to access non-created value");
            return std::move(*static_cast<base &&>(*this).ptr());
        }
    };

    // copy construct
    template <typename T, size_t len = sizeof(T), size_t align = alignof(T),
        bool = std::is_copy_constructible<T>::value, bool = std::is_trivially_copy_constructible<T>::value>
    struct TS_EMPTY_BASES inplace_storage_copy_construct_base : inplace_storage_base<T, len, align> {
        using inplace_storage_base<T, len, align>::inplace_storage_base;
    };

    template <typename T, size_t len, size_t align>
    struct TS_EMPTY_BASES inplace_storage_copy_construct_base<T, len, align, true, false> : inplace_storage_base<T, len, align> {
        using inplace_storage_base<T, len, align>::inplace_storage_base;
        inplace_storage_copy_construct_base() = default;
        inplace_storage_copy_construct_base(const inplace_storage_copy_construct_base &rhs)
            noexcept(std::is_nothrow_copy_constructible<T>::value) : inplace_storage_base<T, len, align>() {
            if (rhs.has_value()) {
                this->construct(rhs.get());
            }
        }
        inplace_storage_copy_construct_base(inplace_storage_copy_construct_base &&rhs) = default;
        inplace_storage_copy_construct_base &operator=(const inplace_storage_copy_construct_base &rhs) = default;
        inplace_storage_copy_construct_base &operator=(inplace_storage_copy_construct_base &&rhs) = default;
    };

    // move construct
    template <typename T, size_t len = sizeof(T), size_t align = alignof(T),
        bool = std::is_move_constructible<T>::value,
        bool = std::is_trivially_move_constructible<T>::value>
    struct TS_EMPTY_BASES inplace_storage_move_construct_base : inplace_storage_copy_construct_base<T, len, align> {
        using inplace_storage_copy_construct_base<T, len, align>::inplace_storage_copy_construct_base;
    };

    template <typename T, size_t len, size_t align>
    struct TS_EMPTY_BASES inplace_storage_move_construct_base<T, len, align, true,
                false> : inplace_storage_copy_construct_base<T, len, align> {
        using inplace_storage_copy_construct_base<T, len, align>::inplace_storage_copy_construct_base;
        inplace_storage_move_construct_base() = default;
        inplace_storage_move_construct_base(const inplace_storage_move_construct_base &rhs) = default;
        inplace_storage_move_construct_base(inplace_storage_move_construct_base &&rhs)
            noexcept(std::is_nothrow_move_constructible<T>::value) : inplace_storage_copy_construct_base<T, len,
            align>() {
            if (rhs.has_value()) {
                this->construct(std::move(rhs.get()));
            }
        }
        inplace_storage_move_construct_base &operator=(const inplace_storage_move_construct_base &rhs) = default;
        inplace_storage_move_construct_base &operator=(inplace_storage_move_construct_base &&rhs) = default;
    };

    // copy assign
    template <typename T, size_t len = sizeof(T), size_t align = alignof(T),
        bool = std::is_copy_assignable<T>::value,
        bool = std::is_trivially_copy_assignable<T>::value>
    struct TS_EMPTY_BASES inplace_storage_copy_assign_base : inplace_storage_move_construct_base<T, len, align> {
        using inplace_storage_move_construct_base<T, len, align>::inplace_storage_move_construct_base;
    };

    template <typename T, size_t len, size_t align>
    struct TS_EMPTY_BASES inplace_storage_copy_assign_base<T, len, align, true,
                false> : inplace_storage_move_construct_base<T, len, align> {
        using inplace_storage_move_construct_base<T, len, align>::inplace_storage_move_construct_base;
        inplace_storage_copy_assign_base() = default;
        inplace_storage_copy_assign_base(const inplace_storage_copy_assign_base &rhs) = default;
        inplace_storage_copy_assign_base(inplace_storage_copy_assign_base &&rhs) = default;
        inplace_storage_copy_assign_base &operator=(const inplace_storage_copy_assign_base &rhs)
            noexcept(std::is_nothrow_copy_constructible<T>::value) {
            if (this != &rhs) {
                if (!rhs.has_value()) {
                    this->destroy();
                } else {
                    T tmp(rhs.get());
                    this->emplace(std::move(tmp));
                }
            }
            return *this;
        }
        inplace_storage_copy_assign_base &operator=(inplace_storage_copy_assign_base &&rhs) = default;
    };

    // move assign
    template <typename T, size_t len = sizeof(T), size_t align = alignof(T),
        bool = std::is_move_constructible<T>::value,
        bool = std::is_trivially_move_assignable<T>::value>
    struct TS_EMPTY_BASES inplace_storage_move_assign_base : inplace_storage_copy_assign_base<T, len, align> {
        using inplace_storage_copy_assign_base<T, len, align>::inplace_storage_copy_assign_base;
    };

    template <typename T, size_t len, size_t align>
    struct TS_EMPTY_BASES inplace_storage_move_assign_base<T, len, align, true,
                false> : inplace_storage_copy_assign_base<T, len, align> {
        using inplace_storage_copy_assign_base<T, len, align>::inplace_storage_copy_assign_base;
        inplace_storage_move_assign_base() = default;
        inplace_storage_move_assign_base(const inplace_storage_move_assign_base &rhs) = default;
        inplace_storage_move_assign_base(inplace_storage_move_assign_base &&rhs) = default;
        inplace_storage_move_assign_base &operator=(const inplace_storage_move_assign_base &rhs) = default;
        inplace_storage_move_assign_base &operator=(inplace_storage_move_assign_base &&rhs)
            noexcept(std::is_nothrow_move_constructible<T>::value) {
            if (this != &rhs) {
                if (!rhs.has_value()) {
                    this->destroy();
                } else {
                    T tmp(std::move(rhs).get());
                    this->emplace(std::move(tmp));
                }
            }
            return *this;
        }
    };

    template <typename T, size_t len = sizeof(T), size_t align = alignof(T)>
    class TS_EMPTY_BASES inplace_t :
            inplace_storage_move_assign_base<T, len, align>,
            ctor_delete_base<T, conjunction_v<std::is_copy_constructible<T>, can_strong_replace<T> >,
                conjunction_v<std::is_move_constructible<T>, can_strong_replace<T> > >,
            assign_delete_base<T, conjunction_v<std::is_copy_constructible<T>, can_strong_replace<T> >,
                conjunction_v<std::is_move_constructible<T>, can_strong_replace<T> > > {
        static_assert(!std::is_array<T>::value, "putting array into inplace_t is not supported.");
        using base = inplace_storage_move_assign_base<T, len, align>;

        void swap_impl(inplace_t &rhs, std::true_type /*movable*/) {
            if (this == &rhs) {
                return;
            }
            auto tmp = std::move(rhs);
            rhs = std::move(*this);
            *this = std::move(tmp);
        }

        void swap_impl(inplace_t &rhs, std::false_type /*copy-only*/) {
            if (this == &rhs) {
                return;
            }

            auto tmp = rhs;
            rhs = *this;
            *this = tmp;
        }

    public:
        using value_type = T;
        using base::base;

        inplace_t() = default;
        ~inplace_t() = default;

        inplace_t(const inplace_t &) = default;
        inplace_t(inplace_t &&) noexcept = default;

        inplace_t &operator=(const inplace_t &) = default;
        inplace_t &operator=(inplace_t &&) noexcept = default;

        using base::emplace;
        using base::destroy;
        using base::has_value;

        explicit operator bool() const noexcept {
            return has_value();
        }

        T &get() noexcept {
            return static_cast<base &>(*this).get();
        }

        const T &get() const noexcept {
            return static_cast<const base &>(*this).get();
        }

        template <typename U = T,
#if LFNDS_HAS_EXCEPTIONS
            typename = std::enable_if_t<std::is_move_constructible<U>::value>
#else
            typename = std::enable_if_t<std::is_nothrow_move_constructible<U>::value>
#endif
        >
        T steal() noexcept(std::is_nothrow_move_constructible<U>::value) {
            T t = static_cast<base &&>(*this).get();
            this->destroy();
            return std::move(t);
        }

        template <typename U = base, typename =
            std::enable_if_t<std::conditional_t<conjunction_v<std::is_move_constructible<U>, std::is_move_assignable<U>>,
                conjunction<std::is_nothrow_move_constructible<U>, std::is_nothrow_move_assignable<U> >,
                conjunction<std::is_nothrow_copy_constructible<U>, std::is_nothrow_copy_assignable<U> > >::value> >
        void swap(inplace_t &rhs) noexcept {
            if (this == &rhs) {
                return;
            }
            using movable = std::integral_constant<bool, std::is_move_constructible<T>::value>;
            swap_impl(rhs, movable{});
        }
    };

    template <typename T, size_t len1, size_t align1, size_t len2, size_t align2>
    bool operator==(const inplace_t<T, len1, align1> &lhs, const inplace_t<T, len2, align2> &rhs)
        noexcept(noexcept(std::declval<const T &>() == std::declval<const T &>())) {
        if (lhs.has_value() != rhs.has_value()) {
            return false;
        }
        return (!lhs.has_value() && !rhs.has_value()) || (lhs.get() == rhs.get());
    }

    template <typename T, size_t len1, size_t align1, size_t len2, size_t align2>
    bool operator!=(const inplace_t<T, len1, align1> &lhs, const inplace_t<T, len2, align2> &rhs)
        noexcept (noexcept(lhs == rhs)) {
        return !(lhs == rhs);
    }

    template <typename T, size_t len, size_t align>
    void swap(inplace_t<T, len, align> &lhs, inplace_t<T, len, align> &rhs)
        noexcept(noexcept(std::declval<inplace_t<T, len, align>&>().swap(std::declval<inplace_t<T, len, align>&>()))) {
        lhs.swap(rhs);
    }

    template <typename Box>
    struct box_swap_nothrow_ok_impl : public conjunction<
                std::is_nothrow_move_constructible<typename Box::value_type>,
                std::integral_constant<bool, noexcept(std::declval<Box &>().emplace(
                    std::declval<typename Box::value_type &&>()))> > {
    };

    template <typename B1, typename B2>
    struct box_pair_nothrow_swappable
            : conjunction<std::is_same<typename B1::value_type, typename B2::value_type>,
                box_swap_nothrow_ok_impl<B1>, box_swap_nothrow_ok_impl<B2> > {
    };

    template <typename T, size_t L1, size_t A1, size_t L2, size_t A2,
        std::enable_if_t<conjunction_v<
            disjunction<std::integral_constant<bool, L1 != L2>, std::integral_constant<bool, A1 != A2> >,
            box_pair_nothrow_swappable<inplace_t<T, L1, A1>, inplace_t<T, L2, A2> > > >* = nullptr>
    void swap(inplace_t<T, L1, A1> &x, inplace_t<T, L2, A2> &y) noexcept {
        if (&x == &y) {
            return;
        }

        if (y.has_value()) {
            T tmp(std::move(y).get());
            if (x.has_value()) {
                y.emplace(std::move(x).get());
            } else {
                x.emplace(std::move(tmp));
                y.destroy();
                return;
            }
            x.emplace(std::move(tmp));
        } else {
            if (x.has_value()) {
                T tmp(std::move(x).get());
                x.destroy();
                y.emplace(std::move(tmp));
            }
        }
    }
}

#endif
