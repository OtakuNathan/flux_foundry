#ifndef FLUX_FOUNDRY_FLOW_CONTRACT_H
#define FLUX_FOUNDRY_FLOW_CONTRACT_H

#include <cstdio>
#include <cstdlib>
#include "flow_node.h"

// Configure consistently across translation units. Optimized builds can opt in.
#ifndef FLUX_FOUNDRY_ENABLE_VACCINE
# ifdef NDEBUG
#  define FLUX_FOUNDRY_ENABLE_VACCINE 0
# else
#  define FLUX_FOUNDRY_ENABLE_VACCINE 1
# endif
#endif

#ifndef FLUX_FOUNDRY_ENABLE_POISON
# define FLUX_FOUNDRY_ENABLE_POISON FLUX_FOUNDRY_ENABLE_VACCINE
#endif

namespace flux_foundry {
    struct pipeline_position {
        const char* name;
        const char* file;
        unsigned line;
    };

    enum class contract_failure { predicate_false, predicate_threw };
    struct contract_violation {
        pipeline_position position;
        const char* contract;
        contract_failure reason;
    };
    // Hooks must not throw. Returning from a hook still terminates the process.
    using contract_failure_hook = void (*)(const contract_violation&);

    inline void default_contract_failure_hook(const contract_violation& failure) noexcept {
        std::fprintf(stderr, "flux_foundry vaccine: %s at %s (%s:%u): %s\n",
            failure.contract ? failure.contract : "unnamed contract",
            failure.position.name ? failure.position.name : "unpositioned",
            failure.position.file ? failure.position.file : "unknown source",
            failure.position.line,
            failure.reason == contract_failure::predicate_false ? "predicate returned false" : "predicate threw");
        std::fflush(stderr);
    }

    namespace flow_impl {
        [[noreturn]] inline void fail_contract(const contract_violation& failure,
                                               contract_failure_hook hook) noexcept {
            (hook ? hook : default_contract_failure_hook)(failure);
            std::abort();
        }

        struct position_marker {
            pipeline_position location;
            template <typename R>
            void operator()(const R&) const noexcept {}
        };
        struct position_node { pipeline_position location; };
        template <typename Node>
        struct positioned_node { pipeline_position location; Node node; };

        template <typename F>
        struct vaccine_node {
            const char* name;
            F predicate;
            contract_failure_hook hook;
        };
        struct disabled_vaccine_node {};

        template <typename F, typename R, typename = void>
        struct is_contract_predicate : std::false_type {};
        template <typename F, typename R>
        struct is_contract_predicate<F, R, void_t<decltype(std::declval<const F&>()(std::declval<const R&>()))>>
            : conjunction<std::is_same<bool, decltype(std::declval<const F&>()(std::declval<const R&>()))>,
                std::integral_constant<bool, FLUX_FOUNDRY_HAS_EXCEPTIONS ||
                    noexcept(std::declval<const F&>()(std::declval<const R&>()))>> {};

        template <typename F>
        struct contract_observer {
            pipeline_position location;
            const char* name;
            F predicate;
            contract_failure_hook hook;

            template <typename R>
            void operator()(const R& result) const noexcept {
                static_assert(std::is_same<bool, decltype(predicate(result))>::value,
                    "vaccine predicate must accept const result_t& and return bool");
#if !FLUX_FOUNDRY_HAS_EXCEPTIONS
                static_assert(noexcept(predicate(result)), "no-exception vaccine predicate must be noexcept");
#endif
                bool passed = false;
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
                try {
#endif
                    passed = predicate(result);
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
                } catch (...) {
                    fail_contract({location, name, contract_failure::predicate_threw}, hook);
                }
#endif
                if (!passed) fail_contract({location, name, contract_failure::predicate_false}, hook);
            }
        };

        template <typename Node>
        struct is_position_marker : std::false_type {};
        template <typename R>
        struct is_position_marker<flow_observer_node<R, position_marker>> : std::true_type {};

        // Blueprint storage is reversed: the first position found is the nearest
        // preceding source marker. Labels survive fusion and executor handoff.
        template <std::size_t I, std::size_t N>
        struct preceding_position {
            template <typename Storage>
            static constexpr pipeline_position get(const Storage& nodes) noexcept {
                return select(nodes, is_position_marker<flat_storage_element_t<I, Storage>>{});
            }
            template <typename Storage>
            static constexpr pipeline_position select(const Storage& nodes, std::true_type) noexcept {
                return flux_foundry::get<I>(nodes).f.location;
            }
            template <typename Storage>
            static constexpr pipeline_position select(const Storage& nodes, std::false_type) noexcept {
                return preceding_position<I + 1, N>::get(nodes);
            }
        };
        template <std::size_t N>
        struct preceding_position<N, N> {
            template <typename Storage>
            static constexpr pipeline_position get(const Storage&) noexcept { return {nullptr, nullptr, 0}; }
        };

        template <typename I, typename O, typename... Nodes>
        constexpr decltype(auto) operator|(flow_blueprint<I, O, Nodes...>&& bp, position_node marker) {
            static_assert(!is_runnable_bp_v<flow_blueprint<I, O, Nodes...>>, "position must precede end()");
#if FLUX_FOUNDRY_ENABLE_VACCINE || FLUX_FOUNDRY_ENABLE_POISON
            return std::move(bp) | flow_observer_node<O, position_marker>{{marker.location}};
#else
            (void)marker;
            return std::move(bp);
#endif
        }

        template <typename I, typename O, typename... Nodes, typename Node>
        constexpr decltype(auto) operator|(flow_blueprint<I, O, Nodes...>&& bp, positioned_node<Node>&& node) {
            return (std::move(bp) | position_node{node.location}) | std::move(node.node);
        }

        template <typename I, typename O, typename... Nodes, typename F>
        constexpr auto operator|(flow_blueprint<I, O, Nodes...>&& bp, vaccine_node<F>&& node) {
            static_assert(!is_runnable_bp_v<flow_blueprint<I, O, Nodes...>>, "vaccine must precede end()");
            static_assert(is_contract_predicate<F, O>::value,
                "vaccine predicate must accept const result_t&, return bool and be noexcept without exceptions");
            using observer = contract_observer<F>;
            const auto location = preceding_position<0, sizeof...(Nodes)>::get(bp.nodes_);
            return std::move(bp) | flow_observer_node<O, observer>{
                observer{location, node.name, std::move(node.predicate), node.hook}};
        }

        template <typename I, typename O, typename... Nodes>
        constexpr flow_blueprint<I, O, Nodes...>&& operator|(flow_blueprint<I, O, Nodes...>&& bp, disabled_vaccine_node) noexcept {
            static_assert(!is_runnable_bp_v<flow_blueprint<I, O, Nodes...>>, "vaccine must precede end()");
            return std::move(bp);
        }
    }

    // Names and optional source-file strings must outlive every run of the blueprint.
    constexpr auto position(const char* name, const char* file = nullptr, unsigned line = 0) noexcept {
        return flow_impl::position_node{{name, file, line}};
    }

    template <typename Node, std::enable_if_t<!std::is_convertible<Node, const char*>::value, int> = 0>
    constexpr auto position(const char* name, Node&& node) noexcept {
        static_assert(flow_impl::require_nothrow_builder_storage<Node>(), "invalid positioned node storage");
        return flow_impl::positioned_node<std::decay_t<Node>>{{name, nullptr, 0}, std::forward<Node>(node)};
    }

    template <typename F>
    constexpr auto vaccine(const char* name, F&& predicate, contract_failure_hook hook = nullptr) noexcept {
#if FLUX_FOUNDRY_ENABLE_VACCINE
        static_assert(flow_impl::require_nothrow_builder_storage<F>(), "invalid vaccine predicate storage");
        return flow_impl::vaccine_node<std::decay_t<F>>{name, std::forward<F>(predicate), hook};
#else
        (void)name; (void)predicate; (void)hook;
        return flow_impl::disabled_vaccine_node{};
#endif
    }
    template <typename F>
    constexpr auto vaccine(F&& predicate) noexcept {
        return vaccine("vaccine", std::forward<F>(predicate));
    }
}

#define FLUX_FOUNDRY_POSITION(name) ::flux_foundry::position((name), __FILE__, __LINE__)

#endif
