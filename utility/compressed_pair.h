#ifndef __LITE_FNDS_COMPRESSED_PAIR_H__
#define __LITE_FNDS_COMPRESSED_PAIR_H__

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
        private ctor_delete_base<T, std::is_copy_constructible<T>::value, std::is_move_constructible<T>::value>,
        private assign_delete_base<T, std::is_copy_assignable<T>::value, std::is_move_assignable<T>::value> {
    static_assert(!std::is_void<T>::value, "T must not be void");

    using reference_type = T&;
    using value_type = T;
    using const_reference_type = const T&;

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
        typename = std::enable_if_t<std::is_constructible<T_, Args&&...>::value>>
    compressed_pair_element(Args&& ... args)
        noexcept(std::is_nothrow_constructible<T_, Args&&...>::value)
        : _value(std::forward<Args>(args)...) {
    }

    template <typename T_ = T, typename K, typename... Args,
        typename = std::enable_if_t<std::is_constructible<T_, std::initializer_list<K>, Args&&...>::value>>
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

    volatile reference_type get() volatile noexcept {
        return _value;
    }

    volatile const_reference_type get() const volatile noexcept {
        return _value;
    }

    template <typename T_ = T, typename = std::enable_if_t<is_swappable<T_>::value>>
    void swap(compressed_pair_element& rhs) 
        noexcept(noexcept(swap(std::declval<value_type&>(), std::declval<value_type&>()))) {
        using std::swap;
        swap(_value, rhs._value);
    }

    value_type _value;
};

template <typename T, size_t index>
struct TS_EMPTY_BASES compressed_pair_element<T, index, true> : private T,
    private ctor_delete_base<T, std::is_copy_constructible<T>::value, std::is_move_constructible<T>::value>,
    private assign_delete_base<T, std::is_copy_assignable<T>::value, std::is_move_assignable<T>::value> {
    static_assert(!std::is_void<T>::value, "T must not be void");
    using reference_type = T&;
    using value_type = T;
    using const_reference_type = const T&;

    compressed_pair_element(const compressed_pair_element& rhs) 
        noexcept(std::is_nothrow_copy_constructible<T>::value) = default;
    compressed_pair_element(compressed_pair_element&& rhs) 
        noexcept(std::is_nothrow_move_constructible<T>::value) = default;

    compressed_pair_element& operator=(const compressed_pair_element& rhs) 
        noexcept(std::is_nothrow_copy_assignable<T>::value) = default;
    compressed_pair_element& operator=(compressed_pair_element&& rhs) 
        noexcept(std::is_nothrow_move_assignable<T>::value) = default;

    template <typename T_ = T, typename... Args,
        typename = std::enable_if_t<std::is_constructible<T_, Args&&...>::value>>
    compressed_pair_element(Args&&... args) noexcept(std::is_nothrow_constructible<T_, Args&&...>::value)
        : T(std::forward<Args>(args)...) {
    }

    template <typename T_ = T, typename K, typename... Args,
        typename = std::enable_if_t<std::is_constructible<T_, std::initializer_list<K>, Args&&...>::value>>
    compressed_pair_element(std::initializer_list<K> il, Args&&... args) noexcept(std::is_nothrow_constructible<T_, std::initializer_list<K>, Args&&...>::value) 
        : T(il, std::forward<Args>(args)...) {
    }

    reference_type get() noexcept { 
        return *this; 
    }

    const_reference_type get() const noexcept { 
        return *this; 
    }

    volatile reference_type get() volatile noexcept {
        return *this;
    }

    volatile const_reference_type get() const volatile noexcept {
        return *this;
    }

    void swap(compressed_pair_element& element) noexcept { return; }
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
        typename = std::enable_if_t<conjunction_v<std::is_copy_constructible<A__>, std::is_copy_constructible<B__>>>>
    compressed_pair(const _A& a, const _B& b) 
        noexcept(conjunction_v<std::is_nothrow_copy_constructible<A__>, std::is_nothrow_copy_constructible<B__>>)
        : _base0(a) , _base1(b) {
    }

    template <typename A__ = _A, typename B__ = _B,
        typename = std::enable_if_t<conjunction_v<std::is_move_constructible<A__>, std::is_move_constructible<B__>>>>
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

    template <typename = std::enable_if_t<conjunction_v<is_swappable<_A>, is_swappable<_B>>>>
    void swap(compressed_pair& __x) noexcept(
        noexcept(static_cast<_base0&>(*this).swap(static_cast<_base0&>(__x))) && noexcept(static_cast<_base1&>(*this).swap(static_cast<_base1&>(__x))))
    {
        static_cast<_base0&>(*this).swap(static_cast<_base0&>(__x));
        static_cast<_base1&>(*this).swap(static_cast<_base1&>(__x));
    }
};

template <typename _A, typename _B, typename = std::enable_if_t<conjunction_v<is_swappable<_A>, is_swappable<_B>>>>
void swap(compressed_pair<_A, _B>& a, compressed_pair<_A, _B>& b) 
    noexcept(noexcept(std::declval<compressed_pair<_A,_B>&>().swap(std::declval<compressed_pair<_A,_B>&>()))) { 
    return a.swap(b); 
}

}

#endif
