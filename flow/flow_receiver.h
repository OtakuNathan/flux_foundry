//
// Created by Nathan on 02/03/2026.
//

#ifndef FLUX_FOUNDRY_FLOW_RECEIVER_H
#define FLUX_FOUNDRY_FLOW_RECEIVER_H

#include <type_traits>
#include "../memory/result_t.h"

namespace flux_foundry {
    template<typename T, typename D = void>
    struct check_receiver : std::false_type { };

    template <typename T>
    struct check_receiver<T, void_t<typename T::value_type,
            std::enable_if_t<conjunction_v<
                    std::is_nothrow_move_constructible<T>,
                    std::is_nothrow_copy_constructible<T>,
                    is_result_t<typename T::value_type>>
            >>> : std::true_type {
    };

    template <typename T>
    constexpr bool check_receiver_v = check_receiver<T>::value;

    template<typename T, typename R>
    struct is_receiver_compatible {
        template<typename ...>
        static auto check(...) -> std::false_type;


        template<typename T_, typename R_>
        static auto check(int) -> std::integral_constant<bool,
                noexcept(std::declval<R_ &>().emplace(std::declval<T_>()))>;

        constexpr static bool value = decltype(check<T, R>(0))::value;
    };

    template <typename T, typename R>
    constexpr bool is_receiver_compatible_v = is_receiver_compatible<T, R>::value;

    template<typename T>
    struct stub_receiver {
        using value_type = T;
        void emplace(T &&val) noexcept {}
    };

    template<typename T>
    struct is_stub_receiver : std::false_type {
    };

    template<typename T>
    struct is_stub_receiver<stub_receiver<T>> : std::true_type {
    };

    template<typename T>
    constexpr bool is_stub_receiver_v = is_stub_receiver<T>::value;
}

#endif //FLUX_FOUNDRY_FLOW_RECEIVER_H
