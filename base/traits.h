//
// Created by nathan on 2025/8/13.
//

#ifndef LITE_FNDS_TRAITS_H
#define LITE_FNDS_TRAITS_H

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <memory>
#include <cassert>

#ifndef LFNDS_NO_EXCEPTION_STRICT
#define LFNDS_NO_EXCEPTION_STRICT 0
#endif

#define LFNDS_CPP_14 201402L
#define LFNDS_CPP_17 201703L
#define LFNDS_CPP_20 202002L

#if defined(_MSVC_LANG)
    #define LFNDS_CPP_VER _MSVC_LANG
#elif defined(__cplusplus)
    #define LFNDS_CPP_VER __cplusplus
#else
    #define LFNDS_CPP_VER 0L
#endif

#define LFNDS_CPP_AT_LEAST(ver) (LFNDS_CPP_VER >= LFNDS_CPP_##ver)
#define LFNDS_CPP_AT_MOST(ver)  (LFNDS_CPP_VER <= LFNDS_CPP_##ver)

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#  define LFNDS_COMPILER_HAS_EXCEPTIONS 1
#else
#  define LFNDS_COMPILER_HAS_EXCEPTIONS 0
#endif

#ifndef LFNDS_HAS_EXCEPTIONS
#  if !LFNDS_COMPILER_HAS_EXCEPTIONS && LFNDS_NO_EXCEPTION_STRICT
#    define LFNDS_HAS_EXCEPTIONS 0
#  else
#    define LFNDS_HAS_EXCEPTIONS 1
#  endif
#endif

#if LFNDS_CPP_AT_LEAST(20)
#  define LIKELY_IF(expr) if ((expr)) [[likely]]
#  define UNLIKELY_IF(expr) if ((expr)) [[unlikely]]
#else
#  if defined(__GNUC__) || defined(__clang__)
#    define LIKELY_IF(expr) if (__builtin_expect(!!(expr), 1))
#    define UNLIKELY_IF(expr) if (__builtin_expect(!!(expr), 0))
#else
#    define LIKELY_IF(expr)   if (expr)
#    define UNLIKELY_IF(expr) if (expr)
#endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#define FORCE_INLINE inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define FORCE_INLINE __forceinline
#else
#define FORCE_INLINE inline
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define TS_EMPTY_BASES __declspec(empty_bases)
#else
#define TS_EMPTY_BASES
#endif

namespace lite_fnds {
    static constexpr size_t CACHE_LINE_SIZE = 64;

    template <typename T>
    struct is_shared_ptr_impl : std::false_type {};

    template <typename T>
    struct is_shared_ptr_impl<std::shared_ptr<T>> : std::true_type {};

    template <typename T>
    struct is_shared_ptr : is_shared_ptr_impl<std::remove_cv_t<std::remove_reference_t<T>>> {};

    template <typename T>
    constexpr bool is_shared_ptr_v = is_shared_ptr<T>::value;

    template <typename...>
    struct type_list { };

    template <typename T, typename... Args>
    struct is_self_constructing : std::false_type { };

    template <typename T, typename U>
    struct is_self_constructing<T, U> : std::is_same<T, std::decay_t<U>> { };

    template <size_t N>
    struct in_place_index {};

#if LFNDS_CPP_AT_LEAST(17)
    using std::invoke_result;
    using std::invoke_result_t;
#else
    template <typename F, typename ... A>
    using invoke_result = std::result_of<F(A...)>;

    template <typename F, typename ... A>
    using invoke_result_t = std::result_of_t<F(A...)>;
#endif

#if LFNDS_CPP_AT_LEAST(20)
    using std::type_identity;
#else
    template <typename T>
    struct type_identity { using type = T; };
#endif

#if LFNDS_CPP_AT_MOST(14)
    template <typename T>
    struct negation : std::integral_constant<bool, !T::value> {};

    template <typename ...>
    struct conjunction : std::true_type {};

    template <typename head>
    struct conjunction <head> : head {};

    template <typename head, typename ... tail>
    struct conjunction<head, tail...> :
        std::conditional_t<bool(head::value), conjunction<tail...>, head> {
    };

    template <typename ... Ts>
    constexpr bool conjunction_v = conjunction<Ts...>::value;

    template <typename ...>
    struct disjunction : std::false_type {};

    template <typename head>
    struct disjunction <head> : head {};

    template <typename head, typename ... tail>
    struct disjunction<head, tail...> :
        std::conditional_t<static_cast<bool>(head::value), head, disjunction<tail...>> {
    };

    template <typename ... Ts>
    constexpr bool disjunction_v = disjunction<Ts...>::value;

    namespace swap_adl_tests {
        struct tag {};

        template <typename T> 
        tag swap(T &, T &);
        
        template <typename T, std::size_t N> 
        tag swap(T (&a)[N], T (&b)[N]);

        template <typename, typename>
        auto can_swap(...) noexcept(false) -> std::false_type;

        template <typename T, typename U>
        auto can_swap(int) noexcept(noexcept(swap(std::declval<T &>(), std::declval<U &>()))) ->
            decltype(swap(std::declval<T &>(), std::declval<U &>()), std::true_type{});

        template <typename, typename>
        std::false_type uses_std(...);

        template <typename T, typename U>
        std::is_same<decltype(swap(std::declval<T &>(), std::declval<U &>())), tag> uses_std(int);

        template<class T>
        struct is_std_swap_noexcept
                : std::integral_constant<bool,
                    std::is_nothrow_move_constructible<T>::value &&
                    std::is_nothrow_move_assignable<T>::value> {
        };

        template<class T, std::size_t N>
        struct is_std_swap_noexcept<T[N]> : is_std_swap_noexcept<T> {
        };

        template<class T, class U>
        struct is_adl_swap_noexcept
                : std::integral_constant<bool, noexcept(can_swap<T, U>(0))> {
        };
    }

    template<class T, class U = T>
    struct is_swappable
            : std::integral_constant<
                bool,
                decltype(swap_adl_tests::can_swap<T, U>(0))::value 
             && (!decltype(swap_adl_tests::uses_std<T, U>(0))::value 
             || (std::is_move_assignable<T>::value &&
                  std::is_move_constructible<T>::value))> {
    };

    template <typename R, typename... Args>
    struct is_swappable<R (*)(Args...)> : std::true_type { };

    template<class T, std::size_t N>
    struct is_swappable<T[N], T[N]>
            : std::integral_constant<
                bool,
                decltype(swap_adl_tests::can_swap<T[N], T[N]>(0))::value 
                    && (!decltype(swap_adl_tests::uses_std<T[N], T[N]>(0))::value 
                    || is_swappable<T, T>::value)> {
    };

    template<class T, class U = T>
    struct is_nothrow_swappable
            : std::integral_constant<
                bool, is_swappable<T, U>::value &&
                ((decltype(swap_adl_tests::uses_std<T, U>(0))::value &&
                  swap_adl_tests::is_std_swap_noexcept<T>::value) ||
                 (!decltype(swap_adl_tests::uses_std<T, U>(0))::value &&
                  swap_adl_tests::is_adl_swap_noexcept<T, U>::value))> {
    };

    template <typename R, typename... Args>
    struct is_nothrow_swappable<R (*)(Args...)> : std::true_type { };

    template <typename... Us>
    struct is_swappable <type_list<Us...>> : conjunction<is_swappable<Us>...> { };

    template <typename... Us>
    struct is_nothrow_swappable <type_list<Us...>> : conjunction<is_nothrow_swappable<Us>...> { };

    template <typename ... >
    struct void_ { using type = void; };

    template <typename ...  Ts>
    using void_t = typename void_<Ts...>::type;
#else
    using std::void_t;
    using std::conjunction;
    using std::conjunction_v;
    using std::disjunction;
    using std::disjunction_v;
    using std::negation;
    using std::is_swappable;
    using std::is_nothrow_swappable;
#endif

    template <typename T>
    constexpr static bool negation_v = negation<T>::value;

    template <typename T>
    struct can_strong_replace
        : disjunction<std::is_nothrow_move_assignable<T>, std::is_nothrow_copy_assignable<T>,
              std::is_nothrow_move_constructible<T>, std::is_nothrow_copy_constructible<T>> {
    };

    template <typename T>
    struct can_strong_move_or_copy_constructible
        : disjunction<std::is_nothrow_move_constructible<T>, 
            std::is_nothrow_copy_constructible<T>> {
    };

    template <typename T, typename ... Args>
    struct is_aggregate_constructible_impl {
    private:
        template <typename U>
        static auto test(int) -> decltype(U{std::declval<Args>()...}, std::true_type{});

        template <typename ...>
        static auto test(...) -> std::false_type;
    public:
        constexpr static bool value = decltype(test<T>(0))::value;
    };
    template <typename T, typename ... Args>
    struct is_aggregate_constructible : std::integral_constant<bool, is_aggregate_constructible_impl<T, Args...>::value> {};

    template <typename T, typename ... Args>
    struct is_nothrow_aggregate_constructible :
        std::integral_constant<bool, is_aggregate_constructible<T, Args...>::value && noexcept(T{std::declval<Args>()...})> {};

    template <typename T, typename ... Args>
    constexpr bool is_aggregate_constructible_v = is_aggregate_constructible<T, Args...>::value;

    template <typename T, typename ... Args>
    constexpr bool is_nothrow_aggregate_constructible_v = is_nothrow_aggregate_constructible<T, Args...>::value;

    template <typename ... Ts>
    struct is_all_the_same : std::true_type { };

    template <typename H, typename ... Ts>
    struct is_all_the_same<H, Ts...> : conjunction<std::is_same<std::decay_t<H>, std::decay_t<Ts>>...> { };

    template <typename ... Ts>
    constexpr bool is_all_the_same_v = is_all_the_same<Ts...>::value;
}

#endif //__TASK_DEFS_H__
