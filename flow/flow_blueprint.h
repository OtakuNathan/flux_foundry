#ifndef __LITE_FNDS_FLOW_BLUEPRINT_H__
#define __LITE_FNDS_FLOW_BLUEPRINT_H__

#include <atomic>
#include <utility>
#include <tuple>
#include <cassert>

#include "../task/task_wrapper.h"
#include "../task/task_core.h"

namespace lite_fnds {

namespace flow_impl {
    enum class flow_node_type {
        flow_node_calc,
        flow_node_control,
    };

    // this trait is only used to detect the compatibility of flow_blueprint and a new node.
    template <typename G, typename O>
    struct is_invocable_with {
    private:
        template <typename F>
        constexpr static auto detect(int) -> decltype(std::declval<F&>()(std::declval<O>()), std::true_type {});

        template <typename...>
        constexpr static auto detect(...) -> std::false_type;

    public:
        constexpr static bool value = decltype(detect<G>(0))::value;
    };

    template <typename G>
    struct is_invocable_with<G, void> {
    private:
        template <typename F>
        constexpr static auto detect(int) -> decltype(std::declval<F&>()(), std::true_type {});

        template <typename...>
        constexpr static auto detect(...) -> std::false_type;

    public:
        constexpr static bool value = decltype(detect<G>(0))::value;
    };

    template <typename G, typename O>
    struct is_nothrow_invocable_with {
    private:
        template <typename F>
        constexpr static auto detect(int) -> std::integral_constant<bool, noexcept(std::declval<F&>()(std::declval<O>()))>;

        template <typename...>
        constexpr static auto detect(...) -> std::false_type;

    public:
        constexpr static bool value = decltype(detect<G>(0))::value;
    };

    template <typename G>
    struct is_nothrow_invocable_with<G, void> {
    private:
        template <typename F>
        constexpr static auto detect(int) -> std::integral_constant<bool, noexcept(std::declval<F&>()())>;

        template <typename...>
        constexpr static auto detect(...) -> std::false_type;

    public:
        constexpr static bool value = decltype(detect<G>(0))::value;
    };

    // this only for private use
    template <class ... Fs>
    struct zipped_callable {
        using callables = std::tuple<Fs...>;
        callables fs;

        zipped_callable() = delete;

        template <typename = std::enable_if_t<std::is_move_constructible<callables>::value>>
        zipped_callable(callables&& fs_)
            noexcept(std::is_nothrow_move_constructible<callables>::value) : fs(std::move(fs_)) {
        }

        template <typename = std::enable_if_t<std::is_copy_constructible<callables>::value>>
        zipped_callable(const callables& fs_) 
            noexcept(std::is_nothrow_copy_constructible<callables>::value)
            : fs(fs_) {
        }

        template <std::size_t i, std::size_t N>
        struct run {
            template <typename Self, typename X>
            static auto call(Self& self, X&& x)
                -> decltype(run<i + 1, N>::call(self,
                    std::get<i>(self.fs)(std::forward<X>(x)))) {
                return run<i + 1, N>::call(self, std::get<i>(self.fs)(std::forward<X>(x)));
            }
        };

        template <std::size_t N>
        struct run<N, N> {
            template <typename Self, typename X>
            static auto call(Self&, X&& x) {
                return std::forward<X>(x); 
            }
        };

        template <typename X>
        decltype(auto) operator()(X&& x)
            noexcept(noexcept(run<0, sizeof...(Fs)>::call(*this, std::forward<X>(x)))) {
            return run<0, sizeof ... (Fs)>::call(*this, std::forward<X>(x));
        }
    };

    template <typename G, typename ... Fs>
    auto zip_callables(zipped_callable<Fs...> s, G g) {
        return zipped_callable<Fs..., std::decay_t<G>>(
            std::tuple_cat(std::move(s.fs), std::make_tuple(std::move(g))));
    }

    template <class... Fs, typename ... Gs>
    auto zip_callables(zipped_callable<Fs...> lhs, zipped_callable<Gs...> rhs) {
        return zipped_callable<Fs..., Gs...>(std::tuple_cat(std::move(lhs.fs), std::move(rhs.fs)));
    }

    template <typename... Fs>
    auto zip_callables(Fs&&... fs) {
        using Ds = std::tuple<std::decay_t<Fs>...>;
        return zipped_callable<std::decay_t<Fs>...>(Ds { std::forward<Fs>(fs)... });
    }

    // flow calc
    template <typename I, typename O, typename F>
    struct flow_calc_node {
        static constexpr auto kind = flow_node_type::flow_node_calc;

        using F_t = std::decay_t<F>;
        using I_t = I;
        using O_t = O;

        F_t f;

        flow_calc_node(const flow_calc_node&) = default;
        flow_calc_node(flow_calc_node&&) = default;
        flow_calc_node& operator=(const flow_calc_node&) = default;
        flow_calc_node& operator=(flow_calc_node&&) = default;

        explicit flow_calc_node(F_t f_) 
            noexcept(std::is_nothrow_move_constructible<F_t>::value)
            : f(std::move(f_)) {
        }
    };

    template <typename F_I, typename F_O, typename F,
         typename G_I, typename G_O, typename G>
    auto operator|(flow_calc_node<F_I, F_O, F> a, flow_calc_node<G_I, G_O, G> b)
        noexcept(noexcept(zip_callables(std::move(a.f), std::move(b.f)))) {
        using zipped_t = decltype(zip_callables(std::declval<F>(), std::declval<G>()));
        return flow_calc_node<F_I, G_O, zipped_t>(zip_callables(std::move(a.f), std::move(b.f)));
    }

    template <class T>
    struct is_calc_node : std::false_type { };

    template <typename F_I, typename F_O, typename F>
    struct is_calc_node<flow_calc_node<F_I, F_O, F>> : std::true_type { };

    template <typename T>
    using is_calc_node_t = typename is_calc_node<T>::type;

    /// flow control
    template <typename I, typename O, typename P>
    struct flow_control_node {
        static constexpr auto kind = flow_node_type::flow_node_control;
        using I_t = I;
        using O_t = O;
        using P_t = std::decay_t<P>;

        P_t p;

        flow_control_node(const flow_control_node&) = default;
        flow_control_node(flow_control_node&&) = default;
        flow_control_node& operator=(const flow_control_node&) = default;
        flow_control_node& operator=(flow_control_node&&) = default;

        explicit flow_control_node(P_t f_)
            noexcept(std::is_nothrow_move_constructible<P_t>::value)
            : p(std::move(f_)) {
        }
    };

    template <typename T>
    struct is_control_node : std::false_type { };

    template <typename I, typename O, typename P>
    struct is_control_node<flow_control_node<I, O, P>> : std::true_type { };

    template <typename T>
    using is_control_node_t = typename is_control_node<T>::type;

    /// flow end
    template <typename I, typename O, typename F>
    struct flow_end_node {
        static constexpr auto kind = flow_node_type::flow_node_calc;

        using F_t = std::decay_t<F>;
        using I_t = I;
        using O_t = O;

        F_t f;

        flow_end_node(const flow_end_node&) = default;
        flow_end_node(flow_end_node&&) = default;
        flow_end_node& operator=(const flow_end_node&) = default;
        flow_end_node& operator=(flow_end_node&&) = default;

        explicit flow_end_node(F_t f_)
            noexcept(std::is_nothrow_move_constructible<F_t>::value)
            : f(std::move(f_)) {
        }
    };

    template <class T>
    struct is_end_node : std::false_type { };

    template <typename F_I, typename F_O, typename F>
    struct is_end_node<flow_end_node<F_I, F_O, F>> : std::true_type { };

    template <typename T>
    using is_end_node_t = typename is_end_node<T>::type;

    // blueprint
    template <typename I, typename O, typename... Nodes>
    struct flow_blueprint;

    template <typename I, typename O, typename Head, typename... Tail>
    struct flow_blueprint<I, O, Head, Tail...> {
        static_assert(conjunction_v<std::is_nothrow_move_constructible<Head>,
                          std::is_nothrow_move_constructible<Tail>...>,
                          "All nodes in flow_blueprint must be nothrow move-constructible");

        using I_t = I;
        using O_t = O;

        using storage_t = std::tuple<Head, Tail...>;
        storage_t nodes_;
        flow_blueprint() = default;
        explicit flow_blueprint(storage_t&& s) 
            noexcept(std::is_nothrow_move_constructible<storage_t>::value)
            : nodes_(std::move(s)) {
        }

        flow_blueprint(const flow_blueprint&) = delete;
        flow_blueprint& operator=(const flow_blueprint&) = delete;

        flow_blueprint(flow_blueprint&&) noexcept = default;
        flow_blueprint& operator=(flow_blueprint&&) noexcept = default;
        ~flow_blueprint() noexcept = default;
    };

    template <size_t ...>
    struct sequence { };

    template <size_t N, size_t... Ns>
    struct remove_first_idx : remove_first_idx<N - 1, N - 1, Ns...> { };

    template <size_t... idx>
    struct remove_first_idx<1, idx...> {
        using seq = sequence<idx...>;
    };

    template <std::size_t ... I, class Tuple>
    auto remove_first_impl(Tuple&& t, sequence<I...>)
        -> std::tuple<std::tuple_element_t<I, std::decay_t<Tuple>>...> {
        return { std::get<I>(std::forward<Tuple>(t))... };
    }

    template <typename ... Ts>
    auto remove_first(std::tuple<Ts...>&& t)
        ->decltype(remove_first_impl(
            std::forward<std::tuple<Ts...>>(t), typename remove_first_idx<sizeof ... (Ts)>::seq {})) {
        return remove_first_impl(std::forward<std::tuple<Ts...>>(t), 
            typename remove_first_idx<sizeof...(Ts)>::seq{});
    }

    template <typename G_I, typename G_O, typename G,
        typename I, typename O, typename F_I, typename F_O, typename F, typename ... Others>
    auto operator|(flow_blueprint<I, O, flow_calc_node<F_I, F_O, F>, Others...> bp, flow_calc_node<G_I, G_O, G> b) {
        static_assert(is_invocable_with<G, O>::value,
            "calc node is not invocable with current blueprint output type");

        auto first = std::move(std::get<0>(bp.nodes_));
        auto merged = first | std::move(b);

        auto tail = remove_first(std::move(bp.nodes_));
        auto nodes = std::tuple_cat(std::make_tuple(std::move(merged)), std::move(tail));

        return flow_blueprint<I, G_O, decltype(merged), Others...>(std::move(nodes));
    }

    template <typename P_I, typename P_O, typename P,
        typename I, typename O, typename F, typename F_I, typename F_O,  typename ... Others>
    auto operator|(flow_blueprint<I, O, flow_calc_node<F_I, F_O, F>, Others...> bp, flow_control_node<P_I, P_O, P> b) {
        return flow_blueprint<I, O, flow_control_node<P_I, P_O, P>, flow_calc_node<F_I, F_O, F>, Others...>(
            std::tuple_cat(std::make_tuple(std::move(b)), std::move(bp.nodes_))
        );
    }

    template <typename P_I, typename P_O, typename P,
        typename I, typename O, typename ... Others>
    auto operator|(flow_blueprint<I, O, flow_control_node<P_I, P_O, P>, Others...> bp, flow_control_node<P_I, P_O, P>) {
        return bp;
    }

    template <typename P_I, typename P_O, typename P, typename I,
        typename O, typename P_, typename P_I_, typename P_O_, typename ... Others>
    auto operator|(flow_blueprint<I, O, flow_control_node<P_, P_I_, P_O_>, Others...> bp, flow_control_node<P_I, P_O, P> b) {
        auto tail = remove_first(std::move(bp.nodes_));
        return flow_blueprint<I, O, flow_control_node<P_I, P_O, P>, Others...>(
            std::tuple_cat(std::make_tuple(std::move(b)), std::move(tail))
        );
    }

    template <typename F, typename F_I, typename F_O,
        typename I, typename O, typename P_I, typename P_O, typename P, typename... Others>
    auto operator|(flow_blueprint<I, O, flow_control_node<P_I, P_O, P>, Others...> bp, flow_calc_node<F_I, F_O, F> a) {
        static_assert(is_invocable_with<F, O>::value,
            "calc node is not invocable with current blueprint output type");
        auto nodes = std::tuple_cat(std::make_tuple(std::move(a)), std::move(bp.nodes_));
        return flow_blueprint<I, invoke_result_t<F&, O>, flow_calc_node<F_I, F_O, F>, flow_control_node<P_I, P_O, P>, Others...>(
            std::move(nodes)
        );
    }

    template <typename I, typename O, typename F, typename F_I, typename F_O, typename... Others, typename Node>
    auto operator|(flow_blueprint<I, O, flow_end_node<F_I, F_O, F>, Others...>, Node&&) = delete;

    template <typename F, typename F_I, typename F_O, typename I, typename O, typename ... Nodes>
    auto operator|(flow_blueprint<I, O, Nodes...> bp, flow_end_node<F_I, F_O, F> b) {
        static_assert(is_invocable_with<F, O>::value,
            "end node is not invocable with current blueprint output type");
        return flow_blueprint<I, O, flow_end_node<F_I, F_O, F>, Nodes...>(
            std::tuple_cat(std::make_tuple(std::move(b)), std::move(bp.nodes_)));
    }
}

}
#endif
