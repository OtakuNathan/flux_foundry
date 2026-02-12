//
// Created by nathan on 2025/8/13.
//

#ifndef FLUX_FOUNDRY_TRAITS_H
#define FLUX_FOUNDRY_TRAITS_H

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <memory>
#include <atomic>
#include <cassert>

#ifndef FLUEX_FOUNDRY_NO_EXCEPTION_STRICT
#define FLUEX_FOUNDRY_NO_EXCEPTION_STRICT 0
#endif

#define FLUEX_FOUNDRY_CPP_14 201402L
#define FLUEX_FOUNDRY_CPP_17 201703L
#define FLUEX_FOUNDRY_CPP_20 202002L

#if defined(_MSVC_LANG)
    #define FLUEX_FOUNDRY_CPP_VER _MSVC_LANG
#elif defined(__cplusplus)
    #define FLUEX_FOUNDRY_CPP_VER __cplusplus
#else
    #define FLUEX_FOUNDRY_CPP_VER 0L
#endif

#define FLUEX_FOUNDRY_CPP_AT_LEAST(ver) (FLUEX_FOUNDRY_CPP_VER >= FLUEX_FOUNDRY_CPP_##ver)
#define FLUEX_FOUNDRY_CPP_AT_MOST(ver)  (FLUEX_FOUNDRY_CPP_VER <= FLUEX_FOUNDRY_CPP_##ver)

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#  define FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS 1
#else
#  define FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS 0
#endif

#ifndef FLUEX_FOUNDRY_HAS_EXCEPTIONS
#  if !FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS && FLUEX_FOUNDRY_NO_EXCEPTION_STRICT
#    define FLUEX_FOUNDRY_HAS_EXCEPTIONS 0
#  else
#    define FLUEX_FOUNDRY_HAS_EXCEPTIONS 1
#  endif
#endif

#if FLUEX_FOUNDRY_CPP_AT_LEAST(20)
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

#if defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define FLUEX_FOUNDRY_WITH_TSAN 1
#  endif
#endif

#if !defined(FLUEX_FOUNDRY_WITH_TSAN)
#  if defined(__SANITIZE_THREAD__)
#    define FLUEX_FOUNDRY_WITH_TSAN 1
#  else
#    define FLUEX_FOUNDRY_WITH_TSAN 0
#  endif
#endif

namespace flux_foundry {
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

#if FLUEX_FOUNDRY_CPP_AT_LEAST(17)
    using std::invoke_result;
    using std::invoke_result_t;
#else
    template <typename F, typename ... A>
    using invoke_result = std::result_of<F(A...)>;

    template <typename F, typename ... A>
    using invoke_result_t = std::result_of_t<F(A...)>;
#endif

#if FLUEX_FOUNDRY_CPP_AT_LEAST(20)
    using std::type_identity;
#else
    template <typename T>
    struct type_identity { using type = T; };
#endif

#if FLUEX_FOUNDRY_CPP_AT_MOST(14)
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
        using std::swap;

        template <typename, typename>
        auto can_swap(...) noexcept(false) -> std::false_type;

        template <typename T, typename U>
        auto can_swap(int) noexcept(noexcept(swap(std::declval<T>(), std::declval<U>()))
            && noexcept(swap(std::declval<U>(), std::declval<T>())))
            -> decltype(
                swap(std::declval<T>(), std::declval<U>()),
                swap(std::declval<U>(), std::declval<T>()),
                std::true_type{}
            );

        template <typename T, typename U, bool = decltype(can_swap<T, U>(0))::value>
        struct is_nothrow_swappable_helper : std::false_type {
        };

        template <typename T, typename U>
        struct is_nothrow_swappable_helper<T, U, true>
            : std::integral_constant<bool,
                noexcept(swap(std::declval<T>(), std::declval<U>()))
                && noexcept(swap(std::declval<U>(), std::declval<T>()))> {
        };
    }

    template<class T, class U = T>
    struct is_swappable : decltype(swap_adl_tests::can_swap<T&, U&>(0)) {
    };

    template <typename R, typename... Args>
    struct is_swappable<R (*)(Args...)> : std::true_type { };

    template<class T, std::size_t N>
    struct is_swappable<T[N], T[N]> : decltype(swap_adl_tests::can_swap<T(&)[N], T(&)[N]>(0)) {
    };

    template<class T, class U = T>
    struct is_nothrow_swappable : swap_adl_tests::is_nothrow_swappable_helper<T&, U&> {
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
