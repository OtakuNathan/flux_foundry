#ifndef LITE_FNDS_FLAT_STORAGE_H
#define LITE_FNDS_FLAT_STORAGE_H

#include <type_traits>
#include <utility>
#include <initializer_list>
#include <cstddef>

#include "../base/inplace_base.h"

namespace lite_fnds {
    template<typename T, size_t index, bool _is_empty = std::is_empty<T>::value && !std::is_final<T>::value>
    struct TS_EMPTY_BASES flat_storage_leaf :
            private ctor_delete_base<T,
#if LFNDS_HAS_EXCEPTIONS
                    std::is_copy_constructible<T>::value, std::is_move_constructible<T>::value
#else
                    std::is_nothrow_copy_constructible<T>::value, std::is_nothrow_move_constructible<T>::value
#endif
            >,
            private assign_delete_base<T,
#if LFNDS_HAS_EXCEPTIONS
                    std::is_copy_assignable<T>::value, std::is_move_assignable<T>::value
#else
                    std::is_nothrow_copy_assignable<T>::value, std::is_nothrow_move_assignable<T>::value
#endif
            > {
        static_assert(!std::is_void<T>::value, "T must not be void");

        using value_type = T;
        using reference_type = T &;
        using const_reference_type = const T &;
        using rvalue_reference_type = T &&;

        flat_storage_leaf(const flat_storage_leaf &rhs)
            noexcept(std::is_nothrow_copy_constructible<T>::value) = default;

        flat_storage_leaf(flat_storage_leaf &&rhs)
            noexcept(std::is_nothrow_move_constructible<T>::value) = default;

        flat_storage_leaf &operator=(const flat_storage_leaf &rhs)
            noexcept(std::is_nothrow_copy_assignable<T>::value) = default;

        flat_storage_leaf &operator=(flat_storage_leaf &&rhs)
            noexcept(std::is_nothrow_move_assignable<T>::value) = default;

        ~flat_storage_leaf() noexcept = default;

        template<typename T_ = T,
#if LFNDS_HAS_EXCEPTIONS
                std::enable_if_t<std::is_default_constructible<T_>::value
#else
                std::enable_if_t<std::is_nothrow_default_constructible<T_>::value
#endif
                > * = nullptr>
        flat_storage_leaf()
            noexcept(std::is_nothrow_default_constructible<T_>::value) : _value{} {
        }

        template<typename T_ = T, typename... Args, typename = std::enable_if_t<conjunction_v<
                negation<is_self_constructing<flat_storage_leaf, Args&&...>>,
#if LFNDS_HAS_EXCEPTIONS
                std::is_constructible<T_, Args &&...>>
#else
                std::enable_if_t<std::is_nothrow_constructible<T_, Args&&...>>
#endif
        >>
        flat_storage_leaf(Args &&... args)
            noexcept(std::is_nothrow_constructible<T_, Args && ...>::value)
                : _value(std::forward<Args>(args)...) {
        }

        template<typename T_ = T, typename K, typename... Args,
#if LFNDS_HAS_EXCEPTIONS
                typename = std::enable_if_t<std::is_constructible<T_, std::initializer_list<K>, Args &&...>::value>
#else
                typename = std::enable_if_t<std::is_nothrow_constructible<T_, std::initializer_list<K>, Args&&...>::value>
#endif
        >
        flat_storage_leaf(std::initializer_list<K> il, Args &&... args)
        noexcept(std::is_nothrow_constructible<T_, std::initializer_list<K>, Args && ...>::value)
                : _value(il, std::forward<Args>(args)...) {
        }

        reference_type get() & noexcept {
            return _value;
        }

        const_reference_type get() const & noexcept {
            return _value;
        }

        rvalue_reference_type get() && noexcept {
            return std::move(_value);
        }

        template<typename T_ = T,
#if LFNDS_HAS_EXCEPTIONS
                typename = std::enable_if_t<is_swappable<T_>::value>
#else
                typename = std::enable_if_t<is_nothrow_swappable<T_>::value>
#endif
        >
        void swap(flat_storage_leaf &rhs)
        noexcept(is_nothrow_swappable<T_>::value) {
            using std::swap;
            swap(_value, rhs._value);
        }

        value_type _value;
    };

    template<typename T, size_t index>
    struct TS_EMPTY_BASES flat_storage_leaf<T, index, true> :
        private T, private ctor_delete_base<T,
#if LFNDS_HAS_EXCEPTIONS
            std::is_copy_constructible<T>::value, std::is_move_constructible<T>::value
#else
            std::is_nothrow_copy_constructible<T>::value, std::is_nothrow_move_constructible<T>::value
#endif
     >,
         private assign_delete_base<T,
#if LFNDS_HAS_EXCEPTIONS
             std::is_copy_assignable<T>::value, std::is_move_assignable<T>::value
#else
             std::is_nothrow_copy_assignable<T>::value, std::is_nothrow_move_assignable<T>::value
#endif
        > {
        static_assert(!std::is_void<T>::value, "T must not be void");

        using value_type = T;
        using reference_type = T &;
        using const_reference_type = const T &;
        using rvalue_reference_type = T &&;

        flat_storage_leaf(const flat_storage_leaf &rhs)
        noexcept(std::is_nothrow_copy_constructible<T>::value) = default;

        flat_storage_leaf(flat_storage_leaf &&rhs)
        noexcept(std::is_nothrow_move_constructible<T>::value) = default;

        flat_storage_leaf &operator=(const flat_storage_leaf &rhs)
        noexcept(std::is_nothrow_copy_assignable<T>::value) = default;

        flat_storage_leaf &operator=(flat_storage_leaf &&rhs)
        noexcept(std::is_nothrow_move_assignable<T>::value) = default;

        template<typename T_ = T,
#if LFNDS_HAS_EXCEPTIONS
                std::enable_if_t<std::is_default_constructible<T_>::value
#else
                        std::enable_if_t<std::is_nothrow_default_constructible<T_>::value
#endif
                > * = nullptr>
        flat_storage_leaf()
            noexcept(std::is_nothrow_default_constructible<T_>::value) : T{} {
        }

        template<typename T_ = T, typename... Args, typename = std::enable_if_t<conjunction_v<
                negation<is_self_constructing<flat_storage_leaf, Args&&...>>,
#if LFNDS_HAS_EXCEPTIONS
                std::is_constructible<T_, Args &&...>>
#else
                std::enable_if_t<std::is_nothrow_constructible<T_, Args&&...>>
#endif
        >>
        flat_storage_leaf(Args &&... args)
        noexcept(std::is_nothrow_constructible<T_, Args && ...>::value)
                : T(std::forward<Args>(args)...) {
        }

        template<typename T_ = T, typename K, typename... Args,
#if LFNDS_HAS_EXCEPTIONS
                typename = std::enable_if_t<std::is_constructible<T_, std::initializer_list<K>, Args &&...>::value>
#else
                typename = std::enable_if_t<std::is_nothrow_constructible<T_, std::initializer_list<K>, Args&&...>::value>
#endif
        >
        flat_storage_leaf(std::initializer_list<K> il, Args &&... args)
        noexcept(std::is_nothrow_constructible<T_, std::initializer_list<K>, Args && ...>::value)
                : T(il, std::forward<Args>(args)...) {
        }

        reference_type get() & noexcept {
            return static_cast<T &>(*this);
        }

        const_reference_type get() const & noexcept {
            return static_cast<T const &>(*this);
        }

        rvalue_reference_type get() && noexcept {
            return std::move(*this);
        }

        template<typename T_ = T,
#if LFNDS_HAS_EXCEPTIONS
                typename = std::enable_if_t<is_swappable<T_>::value>
#else
                typename = std::enable_if_t<is_nothrow_swappable<T_>::value>
#endif
        >
        void swap(flat_storage_leaf &element)
        noexcept(is_nothrow_swappable<T_>::value) {
            using std::swap;
            swap(static_cast<T &>(*this), static_cast<T &>(element));
        }
    };

    template<typename A_, typename B_>
    struct TS_EMPTY_BASES compressed_pair :
            private flat_storage_leaf<A_, 0>,
            private flat_storage_leaf<B_, 1> {
    private:
        using _base0 = flat_storage_leaf<A_, 0>;
        using _base1 = flat_storage_leaf<B_, 1>;

    public:
        using first_type = typename _base0::value_type;
        using second_type = typename _base1::value_type;

        compressed_pair(const compressed_pair &rhs)
        noexcept(conjunction_v<std::is_nothrow_copy_constructible<A_>, std::is_nothrow_copy_constructible<B_> >)
        = default;

        compressed_pair(compressed_pair &&rhs)
        noexcept(conjunction_v<std::is_nothrow_move_constructible<A_>, std::is_nothrow_move_constructible<B_> >)
        = default;

        compressed_pair &operator=(const compressed_pair &rhs)
        noexcept(conjunction_v<std::is_nothrow_copy_assignable<A_>, std::is_nothrow_copy_assignable<B_> >) = default;

        compressed_pair &operator=(compressed_pair &&rhs)
        noexcept(conjunction_v<std::is_nothrow_move_assignable<A_>, std::is_nothrow_move_assignable<B_> >) = default;

        template<typename T, typename U,
#if LFNDS_HAS_EXCEPTIONS
                typename = std::enable_if_t<conjunction_v<std::is_constructible<A_, T&&>, std::is_constructible<B_, U&&> > >
#else
                typename = std::enable_if_t<conjunction_v<std::is_nothrow_constructible<A_, T&&>, std::is_nothrow_constructible<B_, U&&> > >
#endif
        >
        compressed_pair(T&& a, U&& b)
        noexcept(conjunction_v<std::is_nothrow_constructible<A_, T&&>, std::is_nothrow_constructible<B_, U&&>>)
                : _base0(std::forward<T>(a)), _base1(std::forward<U>(b)) {
        }

        typename _base0::reference_type first() noexcept {
            return static_cast<_base0 &>(*this).get();
        }

        typename _base1::reference_type second() noexcept {
            return static_cast<_base1 &>(*this).get();
        }

        typename _base0::const_reference_type first() const noexcept {
            return static_cast<const _base0 &>(*this).get();
        }

        typename _base1::const_reference_type second() const noexcept {
            return static_cast<const _base1 &>(*this).get();
        }

        template<typename T = A_, typename U = B_,
#if LFNDS_HAS_EXCEPTIONS
                typename = std::enable_if_t<conjunction_v<is_swappable<T>, is_swappable<U> > >
#else
                typename = std::enable_if_t<conjunction_v<is_nothrow_swappable<T>, is_nothrow_swappable<U>>>
#endif
        >
        void swap(compressed_pair &x_) noexcept(
        noexcept(std::declval<_base0 &>().swap(std::declval<_base0 &>()))
        && noexcept(std::declval<_base1 &>().swap(std::declval<_base1 &>()))) {
            static_cast<_base0 &>(*this).swap(static_cast<_base0 &>(x_));
            static_cast<_base1 &>(*this).swap(static_cast<_base1 &>(x_));
        }
    };

    template<typename A_, typename B_,
#if LFNDS_HAS_EXCEPTIONS
            typename = std::enable_if_t<conjunction_v<is_swappable<A_>, is_swappable<B_> > >
#else
            typename = std::enable_if_t<conjunction_v<is_nothrow_swappable<A_>, is_nothrow_swappable<B_>>>
#endif
    >
    void swap(compressed_pair<A_, B_> &a, compressed_pair<A_, B_> &b)
    noexcept(noexcept(std::declval<compressed_pair<A_, B_> &>().swap(std::declval<compressed_pair<A_, B_> &>()))) {
        a.swap(b);
    }

    namespace detail {
        // actually this is a simpler impl of std::tuple, I implemented this because std::tuple on MSVC is like
        // tuple<A, B, C> : public tuple<B, C>
        template<typename Indices, typename... Ts>
        struct flat_storage_base;

        template <size_t... idx, typename... Ts>
        struct TS_EMPTY_BASES flat_storage_base<std::index_sequence<idx...>, Ts...>
                : private flat_storage_leaf<Ts, idx> ... {
        protected:
            template <size_t I>
            struct nth_leaf {
                static_assert(I < sizeof...(Ts), "get<I>: index out of range");

                template <size_t N, typename T>
                static auto pick(flat_storage_leaf<T, N> &) noexcept -> flat_storage_leaf<T, N>;

                using type = decltype(pick<I>(std::declval<flat_storage_base&>()));
            };

        public:
            flat_storage_base() = default;

            template<typename... Us,
#if LFNDS_HAS_EXCEPTIONS
                    typename = std::enable_if_t<conjunction_v<std::is_constructible<flat_storage_leaf<Ts, idx>, Us &&>...> >
#else
                    typename = std::enable_if_t<conjunction_v<std::is_nothrow_constructible<flat_storage_leaf<Ts, idx>, Us&&>...>>
#endif
            >
            flat_storage_base(Us &&... us)
            noexcept(conjunction_v<std::is_nothrow_constructible<flat_storage_leaf<Ts, idx>, Us &&>...>)
                    : flat_storage_leaf<Ts, idx>(std::forward<Us>(us))... {
            }

            flat_storage_base(const flat_storage_base &rhs)
            noexcept(conjunction_v<std::is_nothrow_copy_constructible<flat_storage_leaf<Ts, idx>>...>) = default;

            flat_storage_base(flat_storage_base &&rhs)
            noexcept(conjunction_v<std::is_nothrow_move_constructible<flat_storage_leaf<Ts, idx>>...>) = default;

            flat_storage_base &operator=(const flat_storage_base &rhs)
            noexcept(conjunction_v<std::is_nothrow_copy_assignable<flat_storage_leaf<Ts, idx>>...>) = default;

            flat_storage_base &operator=(flat_storage_base &&rhs)
            noexcept(conjunction_v<std::is_nothrow_move_assignable<flat_storage_leaf<Ts, idx>>...>) = default;

            ~flat_storage_base() = default;

            template<typename T_ = type_list<flat_storage_leaf<Ts, idx>...>,
#if LFNDS_HAS_EXCEPTIONS
                    typename = std::enable_if_t<is_swappable<T_>::value>
#else
                    typename = std::enable_if_t<is_nothrow_swappable<T_>::value>
#endif
            >
            void swap(flat_storage_base &rhs)
            noexcept(is_nothrow_swappable<T_>::value) {
                (void) std::initializer_list<int>{
                        (static_cast<flat_storage_leaf<Ts, idx> &>(*this)
                                .swap(static_cast<flat_storage_leaf<Ts, idx> &>(rhs)), 0)...
                };
            }

            template <size_t I>
            decltype(auto) get() const & noexcept {
                static_assert(I < sizeof...(Ts), "get<I>: index out of range");
                using leaf_type = typename nth_leaf<I>::type;
                return static_cast<const leaf_type&>(*this).get();
            }

            template <size_t I>
            decltype(auto) get() & noexcept {
                static_assert(I < sizeof...(Ts), "get<I>: index out of range");
                using leaf_type = typename nth_leaf<I>::type;
                return static_cast<leaf_type&>(*this).get();
            }

            template <size_t I>
            decltype(auto) get() && noexcept {
                static_assert(I < sizeof...(Ts), "get<I>: index out of range");
                using leaf_type = typename nth_leaf<I>::type;
                return static_cast<leaf_type&&>(*this).get();
            }
        };

        template <size_t... idx, typename... Ts,
#if LFNDS_HAS_EXCEPTIONS
                typename = std::enable_if_t<conjunction_v<is_swappable<flat_storage_leaf<Ts, idx> >...> >
#else
                typename = std::enable_if_t<conjunction_v<is_nothrow_swappable<flat_storage_leaf<Ts, idx>>...>>
#endif
        >
        void swap(flat_storage_base<std::index_sequence<idx...>, Ts...> &a,
                  flat_storage_base<std::index_sequence<idx...>, Ts...> &b)
        noexcept(conjunction_v<is_nothrow_swappable<flat_storage_leaf<Ts, idx> >...>) {
            a.swap(b);
        }
    }

    template <size_t I, typename Storage>
    struct flat_storage_element;

    template <typename ... Ts>
    struct flat_storage : detail::flat_storage_base<std::index_sequence_for<Ts...>, Ts...> {
    private:
        using base_type = detail::flat_storage_base<std::index_sequence_for<Ts...>, Ts...>;
    public:
        using base_type::base_type;
        flat_storage() = default;

        template <size_t I, typename storage>
        friend struct flat_storage_element;
    };

    template <size_t I, typename ... Fs>
    struct flat_storage_element<I, flat_storage<Fs...>> :
        public type_identity<
            typename flat_storage<Fs...>::template nth_leaf<I>::type::value_type
            > {
        static_assert(I < sizeof...(Fs), "get<I>: index out of range");
    };

    template <size_t I, typename Storage>
    using flat_storage_element_t = typename flat_storage_element<I, Storage>::type;

    template <size_t I, typename... Fs>
    decltype(auto) get(flat_storage<Fs...> &storage) noexcept {
        return storage.template get<I>();
    }

    template <size_t I, typename... Fs>
    decltype(auto) get(const flat_storage<Fs...> &storage) noexcept {
        return storage.template get<I>();
    }

    template<size_t I, typename... Fs>
    decltype(auto) get(flat_storage<Fs...> &&storage) noexcept {
        return std::move(storage).template get<I>();
    }

    template <typename ... Args>
    auto make_flat_storage(Args && ... args)
        noexcept(std::is_nothrow_constructible<flat_storage<std::decay_t<Args>...>, Args&&...>::value) {
        return flat_storage<std::decay_t<Args>...>(std::forward<Args>(args)...);
    }
}

#endif
