#ifndef FLUX_FOUNDRY_FLOW_BLUEPRINT_H
#define FLUX_FOUNDRY_FLOW_BLUEPRINT_H

#include <utility>

#include "../memory/flat_storage.h"
#include "../task/task_wrapper.h"
#include "../base/traits.h"

#include "flow_def.h"

namespace flux_foundry {
    namespace flow_impl {
        struct inline_executor {
            void dispatch(task_wrapper_sbo&& sbo) noexcept {
                sbo();
            }

            static inline_executor* executor() noexcept {
                static inline_executor exec;
                return &exec;
            }
        };

        template <typename T, size_t... I, typename ... Ts>
        auto flat_storage_prepend_impl(T&& n, flat_storage<Ts...>&& t, std::index_sequence<I...>) {
            return flat_storage<std::decay_t<T>, Ts...>(std::forward<T>(n), get<I>(std::move(t))...);
        }

        // actually this is designed to store bp nodes, because tuple_cat is toooooo complicated.
        template <typename T, typename ... Ts>
        auto flat_storage_prepend(T&& n, flat_storage<Ts...>&& t) {
            return flat_storage_prepend_impl(std::forward<T>(n), std::move(t),
                                             std::make_index_sequence<sizeof ... (Ts)>{});
        }

        // this trait is only used to detect the compatibility of flow_blueprint and a new node.
        template <typename G, typename O>
        struct is_invocable_with {
        private:
            template <typename F>
            constexpr static auto detect(int) -> decltype(std::declval<F &>()(std::declval<O>()), std::true_type{});

            template <typename...>
            constexpr static auto detect(...) -> std::false_type;

        public:
            constexpr static bool value = decltype(detect<G>(0))::value;
        };

        template <typename G>
        struct is_invocable_with<G, void> {
        private:
            template <typename F>
            constexpr static auto detect(int) -> decltype(std::declval<F &>()(), std::true_type{});

            template <typename...>
            constexpr static auto detect(...) -> std::false_type;

        public:
            constexpr static bool value = decltype(detect<G>(0))::value;
        };

        template <typename G, typename O>
        struct is_nothrow_invocable_with {
        private:
            template <typename F>
            constexpr static auto
            detect(int) -> std::integral_constant<bool, noexcept(std::declval<F &>()(std::declval<O>()))>;

            template <typename...>
            constexpr static auto detect(...) -> std::false_type;

        public:
            constexpr static bool value = decltype(detect<G>(0))::value;
        };

        template <typename G>
        struct is_nothrow_invocable_with<G, void> {
        private:
            template <typename F>
            constexpr static auto detect(int) -> std::integral_constant<bool, noexcept(std::declval<F &>()())>;

            template <typename...>
            constexpr static auto detect(...) -> std::false_type;

        public:
            constexpr static bool value = decltype(detect<G>(0))::value;
        };

        // this only for private use
        template <typename F, typename G>
        struct zipped_callable {
        public:
            using F_T = std::decay_t<F>;
            using G_T = std::decay_t<G>;

            zipped_callable() = delete;

            template <typename F_T_ = F_T, typename G_T_ = G_T>
            zipped_callable(F_T_ &&f_, G_T_ &&g_)
            noexcept(conjunction_v<std::is_nothrow_constructible<F_T, F_T_ &&>,
                    std::is_nothrow_constructible<G_T, G_T_ &&>>)
                    : fg(std::forward<F_T_>(f_), std::forward<G_T_>(g_)) {
            }

            template <typename X>
            auto operator()(X &&x)
            noexcept(noexcept(std::declval<G_T &>()(std::declval<F_T &>()(std::forward<X>(x))))) {
                return fg.second()(fg.first()(std::forward<X>(x)));
            }

            template <typename X>
            auto operator()(X &&x) const
            noexcept(noexcept(std::declval<const G_T &>()(std::declval<const F_T &>()(std::forward<X>(x))))) {
                return fg.second()(fg.first()(std::forward<X>(x)));
            }

        private:
            compressed_pair<F_T, G_T> fg;
        };

        template <typename F, typename G>
        auto zip_callables(F &&f, G &&g)
        noexcept(std::is_nothrow_constructible<
                zipped_callable<std::decay_t<F>, std::decay_t<G>>, F &&, G &&>::value) {
            return zipped_callable<std::decay_t<F>, std::decay_t<G>>(
                    std::forward<F>(f), std::forward<G>(g)
            );
        }

        template <typename F, typename G, typename... Os>
        auto zip_callables(F &&f, G &&g, Os &&... os) {
            return zip_callables(zip_callables(std::forward<F>(f), std::forward<G>(g)), std::forward<Os>(os)...);
        }

        using node_tag_unknown = std::integral_constant<size_t, 0>;
        using node_tag_calc = std::integral_constant<size_t, 1>;
        using node_tag_via = std::integral_constant<size_t, 2>;
        using node_tag_async = std::integral_constant<size_t, 3>;
        using node_tag_end = std::integral_constant<size_t, 4>;

        // flow calc
        template <typename I, typename O, typename F, size_t N = 1>
        struct flow_calc_node {
            using tag = node_tag_calc;
            using F_t = std::decay_t<F>;
            using I_t = I;
            using O_t = O;

            F_t f;

            flow_calc_node(const flow_calc_node &) = default;

            flow_calc_node(flow_calc_node &&) = default;

            flow_calc_node &operator=(const flow_calc_node &) = default;

            flow_calc_node &operator=(flow_calc_node &&) = default;

            explicit flow_calc_node(F_t f_)
            noexcept(std::is_nothrow_move_constructible<F_t>::value)
                    : f(std::move(f_)) {
            }
        };

        template <typename F_I, typename F_O, typename F, size_t F_N,
                typename G_I, typename G_O, typename G>
        auto operator|(flow_calc_node<F_I, F_O, F, F_N> a, flow_calc_node<G_I, G_O, G> b)
        noexcept(noexcept(zip_callables(std::move(a.f), std::move(b.f)))) {
            using zipped_t = decltype(zip_callables(std::declval<F>(), std::declval<G>()));
            return flow_calc_node<F_I, G_O, zipped_t, F_N + 1>(zip_callables(std::move(a.f), std::move(b.f)));
        }

        // flow control
        template <typename I, typename O, typename D>
        struct flow_via_node {
            using tag = node_tag_via;
            using I_t = I;
            using O_t = O;
            using D_t = std::decay_t<D>;

            D_t dispatcher;

            flow_via_node(const flow_via_node &) = default;

            flow_via_node(flow_via_node &&) = default;

            flow_via_node &operator=(const flow_via_node &) = default;

            flow_via_node &operator=(flow_via_node &&) = default;

            explicit flow_via_node(D_t f_)
            noexcept(std::is_nothrow_move_constructible<D_t>::value)
                    : dispatcher(std::move(f_)) {
            }
        };

        // flow async node
        template <typename I, typename O, typename Dispatcher, typename Adaptor, typename DelegateFactory>
        struct flow_async_node {
            using tag = node_tag_async;
            using I_t = I;
            using O_t = O;

            using D_t = std::decay_t<Dispatcher>;
            using A_t = std::decay_t<Adaptor>;
            using Df_t = std::decay_t<DelegateFactory>;

            flat_storage<D_t, A_t, Df_t> data;

            flow_async_node(const flow_async_node&) = default;
            flow_async_node(flow_async_node&&) = default;

            flow_async_node& operator=(const flow_async_node&) = default;
            flow_async_node& operator=(flow_async_node&&) = default;

            explicit flow_async_node(D_t d_, A_t a_, Df_t df_)
                noexcept(conjunction_v<
                    std::is_nothrow_move_constructible<D_t>,
                    std::is_nothrow_move_constructible<A_t>,
                    std::is_nothrow_move_constructible<Df_t>
                >)
                : data(std::move(d_), std::move(a_), std::move(df_)) {
            }

            D_t& dispatcher() noexcept { return get<0>(data); }
            A_t& adaptor() noexcept { return get<1>(data); }
            Df_t& factory() noexcept { return get<2>(data); }

            const D_t& dispatcher() const noexcept { return get<0>(data); }
            const A_t& adaptor() const noexcept { return get<1>(data); }
            const Df_t& factory() const noexcept { return get<2>(data); }
        };

        // flow end
        template <typename I, typename O, typename F>
        struct flow_end_node {
            using tag = node_tag_end;

            using F_t = std::decay_t<F>;
            using I_t = I;
            using O_t = O;

            F_t f;

            flow_end_node(const flow_end_node &) = default;

            flow_end_node(flow_end_node &&) = default;

            flow_end_node &operator=(const flow_end_node &) = default;

            flow_end_node &operator=(flow_end_node &&) = default;

            explicit flow_end_node(F_t f_)
                noexcept(std::is_nothrow_move_constructible<F_t>::value)
                    : f(std::move(f_)) {
            }
        };

        // blueprint
        template <typename I, typename O, typename ... Nodes>
        struct flow_blueprint {
            using I_t = I;
            using O_t = O;
            static constexpr size_t node_count = sizeof ... (Nodes);
        };

        template <typename I, typename O, typename Head, typename ... Tail>
        struct flow_blueprint<I, O, Head, Tail...> {
            static_assert(conjunction_v<std::is_nothrow_move_constructible<Head>,
                                  std::is_nothrow_move_constructible<Tail>...>,
                          "All nodes in flow_blueprint must be nothrow move-constructible");

            using I_t = I;
            using O_t = O;
            static constexpr size_t node_count = 1 + sizeof ... (Tail);

            using storage_t = flat_storage<Head, Tail...>;
            storage_t nodes_;

            flow_blueprint() = default;

            explicit flow_blueprint(storage_t &&s)
                noexcept(std::is_nothrow_move_constructible<storage_t>::value)
                    : nodes_(std::move(s)) {
            }

            flow_blueprint(const flow_blueprint &) = delete;

            flow_blueprint &operator=(const flow_blueprint &) = delete;

            flow_blueprint(flow_blueprint &&) noexcept = default;

            flow_blueprint &operator=(flow_blueprint &&) noexcept = default;

            ~flow_blueprint() noexcept = default;
        };

        template <typename T>
        struct is_blueprint_impl : std::false_type {};

        template <typename I, typename O, typename ... Nodes>
        struct is_blueprint_impl<flow_blueprint<I, O, Nodes...>> : std::true_type {};

        template <typename T>
        struct is_blueprint : is_blueprint_impl<T> {};

        template <typename T>
        constexpr bool is_blueprint_v = is_blueprint<T>::value;

        template <typename BP>
        struct is_runnable_bp {
            template <typename ...>
            static auto check(...) -> std::false_type;

            template <typename bp_t>
            static auto check(int) ->
                conjunction<is_blueprint<bp_t>, std::integral_constant<bool, bool(bp_t::node_count)>,
                    std::is_same<typename flat_storage_element_t<0, typename bp_t::storage_t>::tag, node_tag_end>>;
            constexpr static bool value = decltype(check<BP>(0))::value;
        };

        template <typename T>
        constexpr bool is_runnable_bp_v = is_runnable_bp<T>::value;

        template <std::size_t ... I, typename ... Ts>
        auto remove_first_impl(flat_storage<Ts...> &&t, std::index_sequence<I...>)
            -> flat_storage<flat_storage_element_t<I + 1, flat_storage<Ts...>>...> {
            return make_flat_storage(get<I + 1>(std::move(t))...);
        }

        template <typename ... Ts>
        auto remove_first(flat_storage<Ts...> &&t) ->
            decltype(remove_first_impl(std::declval<flat_storage<Ts...>>(),
                                       std::declval<std::make_index_sequence<sizeof ... (Ts) - 1>>())) {
            return remove_first_impl(std::move(t), std::make_index_sequence<sizeof ... (Ts) - 1>{});
        }

        // calc | calc
        template <typename G_I, typename G_O, typename G,
                typename I, typename O, typename F_I, typename F_O, typename F, size_t N, typename ... Others>
        auto operator|(flow_blueprint<I, O, flow_calc_node<F_I, F_O, F, N>, Others...>&& bp, flow_calc_node<G_I, G_O, G>&& b) {
            static_assert(is_invocable_with<G, O>::value,
                          "calc node is not invocable with current blueprint output type");
            auto merged = get<0>(std::move(bp.nodes_)) | std::move(b);
            auto nodes = flat_storage_prepend(std::move(merged), remove_first(std::move(bp.nodes_)));

            return flow_blueprint<I, G_O, decltype(merged), Others...>(std::move(nodes));
        }

        template <typename G_I, typename G_O, typename G,
                typename I, typename O, typename F_I, typename F_O, typename F, typename... Others>
        auto operator|(flow_blueprint<I, O, flow_calc_node<F_I, F_O, F, MAX_ZIP_N>, Others...>&& bp, flow_calc_node<G_I, G_O, G>&& b) {
            static_assert(is_invocable_with<G, O>::value,
                          "calc node is not invocable with current blueprint output type");
            return flow_blueprint<I, G_O, flow_calc_node<G_I, G_O, G>, flow_calc_node<F_I, F_O, F, MAX_ZIP_N>, Others...>(
                    flat_storage_prepend(std::move(b), std::move(bp.nodes_))
            );
        }

        // calc | via
        template <typename P_I, typename P_O, typename P,
                typename I, typename O, typename F, typename F_I, typename F_O, size_t N, typename... Others>
        auto operator|(flow_blueprint<I, O, flow_calc_node<F_I, F_O, F, N>, Others...>&& bp, flow_via_node<P_I, P_O, P>&& b) {
            return flow_blueprint<I, O, flow_via_node<P_I, P_O, P>, flow_calc_node<F_I, F_O, F, N>, Others...>(
                    flat_storage_prepend(std::move(b), std::move(bp.nodes_))
            );
        }

        // calc | async
        template <typename A_I, typename A_O, typename A_E, typename A_A, typename A_DF,
            typename I, typename O, typename F, typename F_I, typename F_O, size_t N, typename... Others>
        auto operator|(flow_blueprint<I, O, flow_calc_node<F_I, F_O, F, N>, Others...>&& bp, flow_async_node<A_I, A_O, A_E, A_A, A_DF>&& b)
        {
            static_assert(is_invocable_with<A_DF, O>::value,
                "async node's delegate factory doesn't accept with the current blueprint's output type");
            return flow_blueprint<I, A_O, flow_async_node<A_I, A_O, A_E, A_A, A_DF>, flow_calc_node<F_I, F_O, F, N>, Others...>(
                flat_storage_prepend(std::move(b), std::move(bp.nodes_)));
        }

        // via | via
        template <typename P_I, typename P_O, typename P,
            typename I, typename O, typename P_I_, typename P_O_, typename P_, typename... Others>
        auto operator|(flow_blueprint<I, O, flow_via_node<P_I_, P_O_, P_>, Others...>&& bp,
                       flow_via_node<P_I, P_O, P>&& b) {
            return flow_blueprint<I, O, flow_via_node<P_I, P_O, P>, Others...>(
                    flat_storage_prepend(std::move(b), remove_first(std::move(bp.nodes_)))
            );
        }

        // calc | via
        template <typename F, typename F_I, typename F_O,
                typename I, typename O, typename P_I, typename P_O, typename P, typename ... Others>
        auto operator|(flow_blueprint<I, O, flow_via_node<P_I, P_O, P>, Others...>&& bp, flow_calc_node<F_I, F_O, F>&& a) {
            static_assert(is_invocable_with<F, O>::value,
                          "calc node is not invocable with current blueprint output type");
            return flow_blueprint<I, F_O, flow_calc_node<F_I, F_O, F>, flow_via_node<P_I, P_O, P>, Others...>(
                    flat_storage_prepend(std::move(a), std::move(bp.nodes_))
            );
        }

        // via | async
        template <typename A_I, typename A_O, typename A_E, typename A_A, typename A_DF,
            typename I, typename O, typename V_I, typename V_O, typename V_P, typename... Others>
        auto operator|(flow_blueprint<I, O, flow_via_node<V_I, V_O, V_P>, Others...>&& bp,
            flow_async_node<A_I, A_O, A_E, A_A, A_DF>&& b) {
            static_assert(is_invocable_with<A_DF, O>::value,
                "async node's delegate factory is not invocable with current blueprint output type");
            return flow_blueprint<I, A_O, flow_async_node<A_I, A_O, A_E, A_A, A_DF>, flow_via_node<V_I, V_O, V_P>, Others...>(
                flat_storage_prepend(std::move(b), std::move(bp.nodes_)));
        }

        // async | calc
        template <typename A_I, typename A_O, typename A_E, typename A_A, typename A_DF,
            typename I, typename O, typename F, typename F_I, typename F_O, size_t N, typename... Others>
        auto operator|(flow_blueprint<I, O, flow_async_node<A_I, A_O, A_E, A_A, A_DF>, Others...>&& bp,
            flow_calc_node<F_I, F_O, F, N>&& b) {
            static_assert(is_invocable_with<F, O>::value,
                "calc node is not invocable with current blueprint output type");
            return flow_blueprint<I, F_O, flow_calc_node<F_I, F_O, F, N>,
                    flow_async_node<A_I, A_O, A_E, A_A, A_DF>, Others...>(
                flat_storage_prepend(std::move(b), std::move(bp.nodes_)));
        }

        // async | async
        template <typename A_I, typename A_O, typename A_E, typename A_A, typename A_DF,
            typename I, typename O, typename A_I_, typename A_O_, typename A_E_, typename A_A_, typename A_DF_, typename... Others>
        auto operator|(flow_blueprint<I, O, flow_async_node<A_I_, A_O_, A_E_, A_A_, A_DF_>, Others...>&& bp,
            flow_async_node<A_I, A_O, A_E, A_A, A_DF>&& b) {
            static_assert(is_invocable_with<A_DF, O>::value,
                "async node's delegate factory is not invocable with current blueprint output");
            return flow_blueprint<I, A_O, flow_async_node<A_I, A_O, A_E, A_A, A_DF>,
                        flow_async_node<A_I_, A_O_, A_E_, A_A_, A_DF_>, Others...>(
                flat_storage_prepend(std::move(b), std::move(bp.nodes_)));
        }

        // async | via (delete no need, cause async implies via)
        template <typename A_I, typename A_O, typename A_E, typename A_A, typename A_DF,
            typename I, typename O, typename P_, typename P_I_, typename P_O_, typename... Others>
        auto operator|(flow_blueprint<I, O, flow_async_node<A_I, A_O, A_E, A_A, A_DF>, Others...>&& bp,
            flow_via_node<P_, P_I_, P_O_>&& b) = delete;

        // end | others
        template <typename I, typename O, typename F, typename F_I, typename F_O, typename ... Others, typename Node>
        auto operator|(flow_blueprint<I, O, flow_end_node<F_I, F_O, F>, Others...>, Node &&) = delete;

        // others | end
        template <typename F, typename F_I, typename F_O, typename I, typename O, typename ... Nodes>
        auto operator|(flow_blueprint<I, O, Nodes...>&& bp, flow_end_node<F_I, F_O, F>&& b) {
            static_assert(is_invocable_with<F, O>::value,
                          "end node is not invocable with current blueprint output type");
            return flow_blueprint<I, O, flow_end_node<F_I, F_O, F>, Nodes...>(
                    flat_storage_prepend(std::move(b), std::move(bp.nodes_))
            );
        }
    }
}
#endif
