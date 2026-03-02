//
// Created by Nathan on 02/03/2026.
//

#ifndef FLUX_FOUNDRY_FLOW_FORK_RECEIVER_TMP_H
#define FLUX_FOUNDRY_FLOW_FORK_RECEIVER_TMP_H


#include "flow_blueprint.h"
#include "flow_receiver.h"

namespace flux_foundry {
    namespace detail {
        template <typename T, typename P>
        struct fork_receiver_prob {
            template <typename ... >
            static auto detect(...) -> std::false_type;

            template <typename U>
            static auto detect(int) -> decltype(std::declval<U>().forward(std::declval<P>()), std::true_type{});

            static constexpr bool value = decltype(detect<T>(0))::value;
        };
    }

    template <typename Derived, typename From, typename ... To>
    struct fork_receiver {
        static_assert(conjunction_v<flow_impl::is_blueprint<From>, flow_impl::is_blueprint<To>...>,
                "from and to... must be blueprints");

        using value_type = typename From::O_t;
        static_assert(
#if FLUX_FOUNDRY_HAS_EXCEPTIONS
                std::is_copy_constructible<value_type>::value,
#else
                std::is_nothrow_copy_constructible<value_type>::value,
#endif
                "To support forking, the output of upper blueprint must be copy constructible.");

        static_assert(conjunction_v<std::is_convertible<value_type, typename To::I_t>...>,
                "To support forking, the lower blueprint must can be called by the output of upper blueprints.");

        void emplace(value_type &&val) noexcept {
            static_assert(detail::fork_receiver_prob<Derived, value_type>::value,
                          "the impl class Deriver must have a member function, whose sig is\n "
                          "void forward(result_t<T, E>&& ...) noexcept, in which function\n"
                          "the result_t<T, E> should be copied and dispatched to sub blueprints");

            static_cast<Derived*>(this)->forward(std::move(val));
        }
    };
}

#endif //FLUX_FOUNDRY_FLOW_FORK_RECEIVER_TMP_H
