//
// Created by wufen on 10/6/2025.
//

#ifndef __LITE_FNDS_RESULTT__H__
#define __LITE_FNDS_RESULTT__H__

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

        constexpr explicit error_t(const E &e)
            noexcept(std::is_nothrow_copy_constructible<E>::value) : _error(e) {
        }

        constexpr explicit error_t(E &&e)
            noexcept(std::is_nothrow_move_constructible<E>::value) : _error(std::move(e)) {
        }

        template <typename... Args,
            std::enable_if_t<std::is_constructible<E, Args &&...>::value>* = nullptr>
        constexpr explicit error_t(Args &&... args)
            noexcept(std::is_nothrow_constructible<E, Args &&...>::value) : _error(std::forward<Args>(args)...) {
        }

        template <typename T, typename... Args,
            std::enable_if_t<std::is_constructible<E, std::initializer_list<T>, Args &&...>::value>* = nullptr>
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

    constexpr static in_place_index<0> value_tag{};
    constexpr static in_place_index<1> error_tag{};

    template <typename T, typename E>
    struct result_t : private either_t<T, error_t<E> > {
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

        result_t() = delete;
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

        template <typename U, typename F, typename other_base = typename result_t<U, F>::base,
            std::enable_if_t<conjunction_v<negation<std::is_void<U>>,
                negation<conjunction<std::is_same<T, U>, std::is_same<E, F> > >,
                std::is_constructible<base, other_base &&> > >* = nullptr>
        explicit result_t(result_t<U, F> &&rhs)
            noexcept(noexcept(base(std::declval<other_base &&>())))
            : base(std::move(rhs)._as_base()) {
        }

        template <typename U, typename F, typename other_base = typename result_t<U, F>::base,
            std::enable_if_t<conjunction_v<negation<std::is_void<U>>,
                negation<conjunction<std::is_same<T, U>, std::is_same<E, F> > >,
                std::is_constructible<base, const other_base &> > >* = nullptr>
        explicit result_t(const result_t<U, F> &rhs)
            noexcept(noexcept(base(std::declval<const other_base &>())))
            : base(rhs._as_base()) {
        }

        template <typename U, typename F,
            class other_base = typename result_t<U, F>::base,
            std::enable_if_t<conjunction_v<negation<std::is_void<U>>,
                negation<conjunction<std::is_same<T, U>, std::is_same<E, F> > >,
                std::is_assignable<base&, other_base &&> > >* = nullptr>
        result_t& operator=(result_t<U, F> &&rhs)
            noexcept(noexcept(std::declval<base &>() = std::declval<other_base &&>())) {
            this->_as_base() = std::move(rhs)._as_base();
            return *this;
        }

        template <typename U, typename F, typename other_base = typename result_t<U, F>::base,
            std::enable_if_t<conjunction_v<negation<std::is_void<U>>,
                negation<conjunction<std::is_same<T, U>, std::is_same<E, F> > >,
                std::is_assignable<base&, const other_base &> > >* = nullptr>
        result_t& operator=(const result_t<U, F> &rhs)
            noexcept(noexcept(std::declval<base &>() = std::declval<const other_base &>())) {
            this->_as_base() = rhs._as_base();
            return *this;
        }

        template <typename T_ = T,
            std::enable_if_t<conjunction_v<negation<std::is_void<T_>>,
                std::is_move_constructible<T_>, can_strong_replace<T_>> >* = nullptr>
        result_t& operator=(std::add_rvalue_reference_t<std::decay_t<T_>> t)
            noexcept(noexcept(this->_as_base() = std::move(t))) {
            this->_as_base() = std::move(t);
            return *this;
        }

        template <typename T_ = T,
            std::enable_if_t<conjunction_v<negation<std::is_void<T_>>,
                std::is_copy_constructible<T_>, can_strong_replace<T_>>>* = nullptr>
        result_t& operator=(std::add_lvalue_reference_t<std::decay_t<const T_>> t)
            noexcept(noexcept(this->_as_base() = t)) {
            this->_as_base() = t;
            return *this;
        }

        template <typename F_ = error_t<E>, std::enable_if_t<std::is_move_constructible<F_>::value >* = nullptr>
        result_t& operator=(std::add_rvalue_reference_t<std::decay_t<F_> > err)
            noexcept(noexcept(this->_as_base() = std::move(err))) {
            this->_as_base() = std::move(err);
            return *this;
        }

        template <typename F_ = error_t<E>, std::enable_if_t<std::is_copy_constructible<F_>::value>* = nullptr>
        result_t& operator=(std::add_lvalue_reference_t<std::decay_t<const F_>> err)
            noexcept(noexcept(this->_as_base() = err)) {
            this->_as_base() = err;
            return *this;
        }

        template <typename T_ = T, std::enable_if_t<std::is_void<T_>::value>* = nullptr>
        void emplace_value() noexcept {
            this->emplace_first();
        }

        template <typename T_ = T, typename ... Args,
            std::enable_if_t<conjunction_v<negation<std::is_void<T_>>, std::is_constructible<T_, Args&&...>>>* = nullptr>
        void emplace_value(Args&& ... args)
            noexcept(noexcept (std::declval<base&>().emplace_first(std::declval<Args&&>()...))) {
            this->emplace_first(std::forward<Args>(args)...);
        }

        template <typename F_ = error_t<E>, typename... Args,
            std::enable_if_t<conjunction_v<std::is_constructible<F_, Args &&...>>>* = nullptr>
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
    struct is_result_t : std::false_type{};

    template <typename T, typename E>
    struct is_result_t <result_t<T, E>> : std::true_type{};

    template <class T, class E>
    void swap(result_t<T,E>& a, result_t<T,E>& b) noexcept(noexcept(a.swap(b))) {
        a.swap(b);
    }

}


#endif //RESULTT__H
