#ifndef LITE_FNDS_COMPRESSED_PAIR_H
#define LITE_FNDS_COMPRESSED_PAIR_H

#include <type_traits>
#include <utility>
#include <initializer_list>
#include <cstddef>

#include "../base/inplace_base.h"

#if defined(_MSC_VER) && !defined(__clang__)
    #define TS_EMPTY_BASES __declspec(empty_bases)
#else
    #define TS_EMPTY_BASES
#endif

namespace lite_fnds {
    template <typename T, size_t index, bool _is_empty = std::is_empty<T>::value && !std::is_final<T>::value>
    struct TS_EMPTY_BASES compressed_pair_element : 
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
    using reference_type = T&;
    using const_reference_type = const T&;
    using volatile_reference_type = T volatile&;
    using const_volatile_reference_type = T const volatile&;

    compressed_pair_element(const compressed_pair_element& rhs) 
        noexcept(std::is_nothrow_copy_constructible<T>::value) = default;
    compressed_pair_element(compressed_pair_element&& rhs) 
        noexcept(std::is_nothrow_move_constructible<T>::value) = default;

    compressed_pair_element& operator=(const compressed_pair_element& rhs) 
        noexcept(std::is_nothrow_copy_assignable<T>::value) = default;
    compressed_pair_element& operator=(compressed_pair_element&& rhs) 
        noexcept(std::is_nothrow_move_assignable<T>::value) = default;

    ~compressed_pair_element() noexcept = default;


    template <typename T_ = T, typename ... Args,
#if LFNDS_HAS_EXCEPTIONS
        typename = std::enable_if_t<std::is_constructible<T_, Args&&...>::value>
#else
        typename = std::enable_if_t<std::is_nothrow_constructible<T_, Args&&...>::value>
#endif
            >
    compressed_pair_element(Args&& ... args)
        noexcept(std::is_nothrow_constructible<T_, Args&&...>::value)
        : _value(std::forward<Args>(args)...) {
    }

    template <typename T_ = T, typename K, typename... Args,
#if LFNDS_HAS_EXCEPTIONS
        typename = std::enable_if_t<std::is_constructible<T_, std::initializer_list<K>, Args&&...>::value>
#else
        typename = std::enable_if_t<std::is_nothrow_constructible<T_, std::initializer_list<K>, Args&&...>::value>
#endif
            >
    compressed_pair_element(std::initializer_list<K> il, Args&&... args) 
        noexcept(std::is_nothrow_constructible<T_, std::initializer_list<K>, Args&&...>::value)
        : _value(il, std::forward<Args>(args)...) {
    }

    reference_type get() noexcept { 
        return _value; 
    }

    const_reference_type get() const noexcept { 
        return _value; 
    }
            
    volatile_reference_type get() volatile noexcept {
        return _value;
    }

    const_volatile_reference_type get() const volatile noexcept {
        return _value;
    }

    template <typename T_ = T,
#if LFNDS_HAS_EXCEPTIONS
            typename = std::enable_if_t<is_swappable<T_>::value>
#else
            typename = std::enable_if_t<is_nothrow_swappable<T_>::value>
#endif
            >
    void swap(compressed_pair_element& rhs) 
        noexcept(is_nothrow_swappable<T_>::value) {
        using std::swap;
        swap(_value, rhs._value);
    }

    value_type _value;
};

template <typename T, size_t index>
struct TS_EMPTY_BASES compressed_pair_element<T, index, true> : private T,
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
    using reference_type = T&;
    using const_reference_type = const T&;
    using volatile_reference_type = T volatile&;
    using const_volatile_reference_type = T const volatile&;
        
    compressed_pair_element(const compressed_pair_element& rhs)
        noexcept(std::is_nothrow_copy_constructible<T>::value) = default;
    compressed_pair_element(compressed_pair_element&& rhs) 
        noexcept(std::is_nothrow_move_constructible<T>::value) = default;

    compressed_pair_element& operator=(const compressed_pair_element& rhs) 
        noexcept(std::is_nothrow_copy_assignable<T>::value) = default;
    compressed_pair_element& operator=(compressed_pair_element&& rhs) 
        noexcept(std::is_nothrow_move_assignable<T>::value) = default;

    template <typename T_ = T, typename ... Args,
#if LFNDS_HAS_EXCEPTIONS
        typename = std::enable_if_t<std::is_constructible<T_, Args&&...>::value>
#else
        typename = std::enable_if_t<std::is_nothrow_constructible<T_, Args&&...>::value
#endif
            >
    compressed_pair_element(Args&&... args)
        noexcept(std::is_nothrow_constructible<T_, Args&&...>::value)
        : T(std::forward<Args>(args)...) {
    }

    template <typename T_ = T, typename K, typename... Args,
#if LFNDS_HAS_EXCEPTIONS
        typename = std::enable_if_t<std::is_constructible<T_, std::initializer_list<K>, Args&&...>::value>
#else
        typename = std::enable_if_t<std::is_nothrow_constructible<T_, std::initializer_list<K>, Args&&...>::value>
#endif
            >
    compressed_pair_element(std::initializer_list<K> il, Args&&... args)
        noexcept(std::is_nothrow_constructible<T_, std::initializer_list<K>, Args&&...>::value)
        : T(il, std::forward<Args>(args)...) {
    }

    reference_type get() noexcept { 
        return static_cast<T&>(*this);
    }

    const_reference_type get() const noexcept { 
        return static_cast<T const&>(*this);
    }
    
    volatile_reference_type get() volatile noexcept {
        return static_cast<T volatile&>(*this);
    }

    const_volatile_reference_type get() const volatile noexcept {
        return static_cast<T const volatile&>(*this);
    }

    template <typename T_ = T,
#if LFNDS_HAS_EXCEPTIONS
        typename = std::enable_if_t<is_swappable<T_>::value>
#else
        typename = std::enable_if_t<is_nothrow_swappable<T_>::value>
#endif
        >
    void swap(compressed_pair_element& element)
        noexcept(is_nothrow_swappable<T_>::value) {
        using std::swap;
        swap(static_cast<T&>(*this), static_cast<T&>(element));
    }
};

template <typename _A, typename _B>
struct TS_EMPTY_BASES compressed_pair : 
    private compressed_pair_element<_A, 0>, 
    private compressed_pair_element<_B, 1> {
private:
    using _base0 = compressed_pair_element<_A, 0>;
    using _base1 = compressed_pair_element<_B, 1>;

public:
    using first_type = typename _base0::value_type;
    using second_type = typename _base1::value_type;

    compressed_pair(const compressed_pair& rhs) 
        noexcept(conjunction_v<std::is_nothrow_copy_constructible<_A>, std::is_nothrow_copy_constructible<_B>>) = default;
    
    compressed_pair(compressed_pair&& rhs) 
        noexcept(conjunction_v<std::is_nothrow_move_constructible<_A>, std::is_nothrow_move_constructible<_B>>) = default;
    
    compressed_pair& operator=(const compressed_pair& rhs) 
        noexcept(conjunction_v<std::is_nothrow_copy_assignable<_A>, std::is_nothrow_copy_assignable<_B>>) = default;
   
    compressed_pair& operator=(compressed_pair&& rhs) 
        noexcept(conjunction_v<std::is_nothrow_move_assignable<_A>, std::is_nothrow_move_assignable<_B>>) = default;

    template <typename A__ = _A, typename B__ = _B,
#if LFNDS_HAS_EXCEPTIONS
        typename = std::enable_if_t<conjunction_v<std::is_copy_constructible<A__>, std::is_copy_constructible<B__>>>
#else
        typename = std::enable_if_t<conjunction_v<
            std::is_nothrow_copy_constructible<A__>,
            std::is_nothrow_copy_constructible<B__>>>
#endif
    >
    compressed_pair(const _A& a, const _B& b) 
        noexcept(conjunction_v<std::is_nothrow_copy_constructible<A__>, std::is_nothrow_copy_constructible<B__>>)
        : _base0(a) , _base1(b) {
    }

    template <typename A__ = _A, typename B__ = _B,
#if LFNDS_HAS_EXCEPTIONS
    typename = std::enable_if_t<conjunction_v<std::is_move_constructible<A__>, std::is_move_constructible<B__>>>
#else
    typename = std::enable_if_t<conjunction_v<
        std::is_nothrow_move_constructible<A__>, std::is_nothrow_move_constructible<B__>>>
#endif
    >
    compressed_pair(_A&& a, _B&& b) 
        noexcept(conjunction_v<std::is_nothrow_move_constructible<A__>, std::is_nothrow_move_constructible<B__>>)
        : _base0(std::move(a)) , _base1(std::move(b)) {
    }

    typename _base0::reference_type first() noexcept {
        return static_cast<_base0&>(*this).get();
    }

    typename _base1::reference_type second() noexcept {
        return static_cast<_base1&>(*this).get();
    }

    typename _base0::const_reference_type first() const noexcept { 
        return static_cast<const _base0&>(*this).get(); 
    }

    typename _base1::const_reference_type second() const noexcept { 
        return static_cast<const _base1&>(*this).get();
    }

    template <typename A__ = _A, typename B__ = _B,
#if LFNDS_HAS_EXCEPTIONS
            typename = std::enable_if_t<conjunction_v<is_swappable<A__>, is_swappable<B__>>>
#else
            typename = std::enable_if_t<conjunction_v<is_nothrow_swappable<A__>, is_nothrow_swappable<B__>>>
#endif
    >
    void swap(compressed_pair& __x) noexcept(
        noexcept(std::declval<_base0&>().swap(std::declval<_base0&>()))
        && noexcept(std::declval<_base1&>().swap(std::declval<_base1&>()))) {
        static_cast<_base0&>(*this).swap(static_cast<_base0&>(__x));
        static_cast<_base1&>(*this).swap(static_cast<_base1&>(__x));
    }
};

template <typename _A, typename _B,
#if LFNDS_HAS_EXCEPTIONS
            typename = std::enable_if_t<conjunction_v<is_swappable<_A>, is_swappable<_B>>>
#else
            typename = std::enable_if_t<conjunction_v<is_nothrow_swappable<_A>, is_nothrow_swappable<_B>>>
#endif
        >
void swap(compressed_pair<_A, _B>& a, compressed_pair<_A, _B>& b) 
    noexcept(noexcept(std::declval<compressed_pair<_A,_B>&>().swap(std::declval<compressed_pair<_A,_B>&>()))) { 
    a.swap(b); 
}

}

#endif
