//
// Created by wufen on 10/6/2025.
//

#ifndef LITE_FNDS_RESULTT_H
#define LITE_FNDS_RESULTT_H

#include "either_t.h"

namespace lite_fnds {
    template <typename E>
    class error_t {
        static_assert(!std::is_void<E>::value, "E must not be void");
        static_assert(can_strong_move_or_copy_constructible<E>::value,
                      "E must be nothrow copy constructible or be nothrow move constructible");
        E _error;
    public:
        error_t() = delete;

#if !LFNDS_HAS_EXCEPTIONS
        template <typename E_ = E, std::enable_if_t<std::is_nothrow_copy_constructible<E_>::value>* = nullptr>
#endif
        constexpr explicit error_t(const E &e)
            noexcept(std::is_nothrow_copy_constructible<E>::value) : _error(e) {
        }

#if !LFNDS_HAS_EXCEPTIONS
        template <typename E_ = E, std::enable_if_t<std::is_nothrow_move_constructible<E_>::value>* = nullptr>
#endif
        constexpr explicit error_t(E &&e)
            noexcept(std::is_nothrow_move_constructible<E>::value) : _error(std::move(e)) {
        }

        template <typename... Args,
            std::enable_if_t<conjunction_v<negation<is_self_constructing<error_t, Args&&...>>,
#if LFNDS_HAS_EXCEPTIONS
            std::is_constructible<E, Args &&...>
#else
            std::is_nothrow_constructible<E, Args &&...>
#endif
            >>* = nullptr>
        constexpr explicit error_t(Args &&... args)
            noexcept(std::is_nothrow_constructible<E, Args &&...>::value) : _error(std::forward<Args>(args)...) {
        }

        template <typename T, typename... Args,
            std::enable_if_t<
#if LFNDS_HAS_EXCEPTIONS
            std::is_constructible<E, std::initializer_list<T>, Args &&...>::value
#else
            std::is_nothrow_constructible<E, std::initializer_list<T>, Args &&...>::value
#endif
            >* = nullptr>
        constexpr error_t(std::initializer_list<T> il, Args &&... args)
            noexcept(std::is_nothrow_constructible<E, std::initializer_list<T>, Args &&...>::value) : _error(
            il, std::forward<Args>(args)...) {
        }

        constexpr const E &error() const & noexcept {
            return _error;
        }

        constexpr E &error() & noexcept {
            return _error;
        }

        constexpr E &&error() && noexcept {
            return std::move(_error);
        }
    };

    template <typename E>
    constexpr bool operator==(const error_t<E> &lhs, const error_t<E> &rhs)
        noexcept(noexcept(lhs.error() == rhs.error())) {
        return lhs.error() == rhs.error();
    }

    template <typename E>
    constexpr bool operator!=(const error_t<E> &lhs, const error_t<E> &rhs)
        noexcept(noexcept(lhs.error() == rhs.error())) {
        return !(lhs.error() == rhs.error());
    }

    template <typename E>
    constexpr bool operator<(const error_t<E> &lhs, const error_t<E> &rhs)
        noexcept(noexcept(lhs.error() < rhs.error())) {
        return lhs.error() < rhs.error();
    }

    template <typename E>
    constexpr bool operator<=(const error_t<E> &lhs, const error_t<E> &rhs)
        noexcept(noexcept(lhs.error() <= rhs.error())) {
        return lhs.error() <= rhs.error();
    }

    template <typename E>
    constexpr bool operator>(const error_t<E> &lhs, const error_t<E> &rhs)
        noexcept(noexcept(lhs.error() <= rhs.error())) {
        return !(lhs.error() <= rhs.error());
    }

    template <typename E>
    constexpr bool operator>=(const error_t<E> &lhs, const error_t<E> &rhs)
        noexcept(noexcept(lhs.error() < rhs.error())) {
        return !(lhs.error() < rhs.error());
    }

    template <typename T>
    struct is_error_t_impl : std::false_type{};

    template <typename T>
    struct is_error_t_impl<error_t<T>> : std::true_type {};

    template <typename T>
    struct is_error_t : is_error_t_impl<std::decay_t<T>> {};

    template <typename T>
    constexpr bool is_error_t_v = is_error_t<T>::value;

    constexpr static in_place_index<0> value_tag{};
    constexpr static in_place_index<1> error_tag{};

    template <typename T, typename E>
    struct result_t : private either_t<T, error_t<E>> {
        static_assert(!is_error_t_v<T>, "T must not be an error_t");
    private:
        using base = either_t<T, error_t<E> >;
        using base::has_first;
        using base::emplace_first;
        using base::emplace_second;

        template <typename, typename>
        friend struct result_t;

        base &_as_base() & noexcept {
            return static_cast<base &>(*this);
        }

        const base &_as_base() const & noexcept {
            return static_cast<const base &>(*this);
        }

        base&& _as_base() && noexcept {
            return static_cast<base &&>(*this);
        }

    public:
        using base::base;
        using base::swap;

        using value_type = T;
        using error_type = E;

        result_t(const result_t &) = default;
        result_t(result_t &&) noexcept = default;
        result_t &operator=(const result_t &) = default;
        result_t &operator=(result_t &&) noexcept = default;
        ~result_t() = default;

        bool has_value() const noexcept {
            return has_first();
        }

        bool has_error() const noexcept {
            return !has_value();
        }

        template <typename T_ = T,
            std::enable_if_t<!std::is_void<T_>::value>* = nullptr>
        result_t() = delete;

        template <typename T_, std::enable_if_t<conjunction_v<
            negation<std::is_void<T_>>, negation<is_error_t<T_>>, negation<is_self_constructing<result_t, T_>>,
#if LFNDS_HAS_EXCEPTIONS
            std::is_constructible<T, T_&&>
#else
            std::is_nothrow_constructible<T, T_&&>
#endif
        >>* = nullptr> 
        result_t(T_&& t) 
            noexcept(std::is_nothrow_constructible<base, in_place_index<0>, T_&&>::value)
            : base(value_tag, std::forward<T_>(t)) {
        }

        template <typename T_ = T, 
            std::enable_if_t<std::is_void<T_>::value>* = nullptr>
        result_t() 
            noexcept(std::is_nothrow_constructible<base, in_place_index<0>>::value)
            : base(value_tag) {
        }

        template <bool B =
#if LFNDS_HAS_EXCEPTIONS
            std::is_copy_constructible<error_t<E>>::value
#else
            std::is_nothrow_copy_constructible<error_t<E>>::value
#endif
        , std::enable_if_t<B>* = nullptr>
        result_t(const error_t<E>& e) 
            noexcept(std::is_nothrow_constructible<base, 
                in_place_index<1>, 
                const error_t<E>&>::value)
            : base(error_tag, e) {
        }

        template <bool B = 
#if LFNDS_HAS_EXCEPTIONS
            std::is_move_constructible<error_t<E>>::value
#else
            std::is_nothrow_move_constructible<error_t<E>>::value
#endif
        , std::enable_if_t<B>* = nullptr>
        result_t(error_t<E>&& e) 
            noexcept(std::is_nothrow_constructible<base, in_place_index<1>, error_t<E>&&>::value)
            : base(error_tag, std::move(e)) {
        }

        template <typename U, typename F, typename other_base = typename result_t<U, F>::base,
            std::enable_if_t<conjunction_v<negation<std::is_void<U>>,
                negation<conjunction<std::is_same<T, U>, std::is_same<E, F> > >,
#if LFNDS_HAS_EXCEPTIONS
                std::is_constructible<base, other_base &&>
#else
                std::is_constructible<base, other_base &&>
#endif
        > >* = nullptr>
        explicit result_t(result_t<U, F> &&rhs)
            noexcept(noexcept(base(std::declval<other_base &&>())))
            : base(std::move(rhs)._as_base()) {
        }

        template <typename U, typename F, typename other_base = typename result_t<U, F>::base,
            std::enable_if_t<conjunction_v<negation<std::is_void<U>>,
                negation<conjunction<std::is_same<T, U>, std::is_same<E, F> > >,
#if LFNDS_HAS_EXCEPTIONS
                std::is_constructible<base, const other_base &>
#else
                std::is_nothrow_constructible<base, const other_base &>
#endif
        > >* = nullptr>
        explicit result_t(const result_t<U, F> &rhs)
            noexcept(noexcept(base(std::declval<const other_base &>())))
            : base(rhs._as_base()) {
        }

        template <typename U, typename F,
            class other_base = typename result_t<U, F>::base,
            std::enable_if_t<conjunction_v<negation<std::is_void<U>>,
                negation<conjunction<std::is_same<T, U>, std::is_same<E, F> > >,
#if LFNDS_HAS_EXCEPTIONS
                std::is_assignable<base&, other_base &&>
#else
                std::is_nothrow_assignable<base&, other_base &&>
#endif
        > >* = nullptr>
        result_t& operator=(result_t<U, F> &&rhs)
            noexcept(noexcept(std::declval<base &>() = std::declval<other_base &&>())) {
            this->_as_base() = std::move(rhs)._as_base();
            return *this;
        }

        template <typename U, typename F, typename other_base = typename result_t<U, F>::base,
            std::enable_if_t<conjunction_v<negation<std::is_void<U>>,
                negation<conjunction<std::is_same<T, U>, std::is_same<E, F> > >,
#if LFNDS_HAS_EXCEPTIONS
                std::is_assignable<base&, const other_base &>
#else
                std::is_nothrow_assignable<base&, const other_base &>
#endif
        > >* = nullptr>
        result_t& operator=(const result_t<U, F> &rhs)
            noexcept(noexcept(std::declval<base &>() = std::declval<const other_base &>())) {
            this->_as_base() = rhs._as_base();
            return *this;
        }

        template <typename T_ = T,
            std::enable_if_t<conjunction_v<negation<std::is_void<T_>>,
#if LFNDS_HAS_EXCEPTIONS
                std::is_move_constructible<T_>, can_strong_replace<T_>
#else
                std::is_nothrow_move_constructible<T_>
#endif
        >>* = nullptr>
        result_t& operator=(std::add_rvalue_reference_t<std::decay_t<T_>> t)
            noexcept(noexcept(std::declval<base&>() = std::move(t))) {
            this->_as_base() = std::move(t);
            return *this;
        }

        template <typename T_ = T,
            std::enable_if_t<conjunction_v<negation<std::is_void<T_>>,
#if LFNDS_HAS_EXCEPTIONS
                std::is_copy_constructible<T_>, can_strong_replace<T_>
#else
                std::is_nothrow_copy_constructible<T_>
#endif
        >>* = nullptr>
        result_t& operator=(std::add_lvalue_reference_t<std::decay_t<const T_>> t)
            noexcept(noexcept(std::declval<base&>() = t)) {
            this->_as_base() = t;
            return *this;
        }

        template <typename F_ = error_t<E>, std::enable_if_t<
#if LFNDS_HAS_EXCEPTIONS
            std::is_move_constructible<F_>::value
#else
            std::is_nothrow_move_constructible<F_>::value
#endif
        >* = nullptr>
        result_t& operator=(std::add_rvalue_reference_t<std::decay_t<F_> > err)
            noexcept(noexcept(std::declval<base&>() = std::move(err))) {
            this->_as_base() = std::move(err);
            return *this;
        }

        template <typename F_ = error_t<E>, std::enable_if_t<
#if LFNDS_HAS_EXCEPTIONS
            std::is_copy_constructible<F_>::value
#else
            std::is_nothrow_copy_constructible<F_>::value
#endif
        >* = nullptr>
        result_t& operator=(std::add_lvalue_reference_t<std::decay_t<const F_>> err)
            noexcept(noexcept(std::declval<base&>() = err)) {
            this->_as_base() = err;
            return *this;
        }

        template <typename T_ = T, std::enable_if_t<std::is_void<T_>::value>* = nullptr>
        void emplace_value() noexcept {
            this->emplace_first();
        }

        template <typename T_ = T, typename ... Args,
            std::enable_if_t<conjunction_v<negation<std::is_void<T_>>,
#if LFNDS_HAS_EXCEPTIONS
                std::is_constructible<T_, Args&&...>
#else
                std::is_nothrow_constructible<T_, Args&&...>
#endif
            >>* = nullptr>
        void emplace_value(Args&& ... args)
            noexcept(noexcept (std::declval<base&>().emplace_first(std::declval<Args&&>()...))) {
            this->emplace_first(std::forward<Args>(args)...);
        }

        template <typename F_ = error_t<E>, typename... Args,
            std::enable_if_t<conjunction_v<
#if LFNDS_HAS_EXCEPTIONS
            std::is_constructible<F_, Args &&...>
#else
            std::is_nothrow_constructible<F_, Args &&...>
#endif
            >>* = nullptr>
        void emplace_error(Args &&... args)
            noexcept(noexcept (std::declval<base&>().emplace_second(std::declval<Args&&>()...))) {
            this->emplace_second(std::forward<Args>(args)...);
        }

        template <typename T_ = T, std::enable_if_t<!std::is_void<T_>::value>* = nullptr>
        T_ &value() & noexcept {
            return base::get_first();
        }

        template <typename T_ = T, std::enable_if_t<!std::is_void<T_>::value>* = nullptr>
        const T_ &value() const & noexcept {
            return base::get_first();
        }

        template <typename T_ = T, std::enable_if_t<!std::is_void<T_>::value>* = nullptr>
        T_ &&value() && noexcept {
            return std::move(static_cast<base &&>(*this).get_first());
        }

        E &error() & noexcept {
            return base::get_second().error();
        }

        const E &error() const & noexcept {
            return base::get_second().error();
        }

        E &&error() && noexcept {
            return std::move(static_cast<base &&>(*this).get_second().error());
        }
    };

    template <typename R>
    struct is_result_impl : std::false_type {};

    template <typename T, typename E>
    struct is_result_impl<result_t<T, E>> : std::true_type {};

    template <typename R>
    struct is_result_t : is_result_impl<std::decay_t<R>> {};

    template <typename R>
    constexpr bool is_result_t_v = is_result_t<R>::value;

    template <typename T, class E>
    void swap(result_t<T,E>& a, result_t<T,E>& b)
        noexcept(noexcept(std::declval<result_t<T, E>&>().swap(std::declval<result_t<T, E>&>()))) {
        a.swap(b);
    }

}


#endif //__LITE_FNDS_RESULTT_H__
