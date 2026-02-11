#ifndef FLUX_FOUNDRY_FLOW_NODE_H
#define FLUX_FOUNDRY_FLOW_NODE_H

#include "flow_async_aggregator.h"
#include "../task/task_wrapper.h"
#include "flow_blueprint.h"
#include "flow_awaitable.h"

namespace flux_foundry {
    namespace flow_impl {
        struct identity {
            template <typename T>
            constexpr decltype(auto) operator()(T&& t) const noexcept {
                return std::forward<T>(t);
            }
        };

        template <typename Executor>
        struct check_executor {
            template <typename U>
            static auto detect(int) -> std::integral_constant<bool,
                noexcept(std::declval<U&>()->dispatch(std::declval<task_wrapper_sbo>()))>;

            template <typename...>
            static auto detect(...) -> std::false_type;

            static constexpr bool value = decltype(detect<Executor>(0))::value;
        };

        template <typename T, typename... Rest>
        struct is_all_the_same : std::true_type { };

        template <typename T, typename First, typename... Rest>
        struct is_all_the_same<T, First, Rest...>
            : std::integral_constant<bool, std::is_same<T, First>::value && is_all_the_same<T, Rest...>::value> { };

        template <typename F, typename G, typename... Args_>
        struct check_when_all_success_compatibility {
            template <typename...>
            static auto check_success(...) -> std::false_type;

            template <typename F_, typename... As>
            static auto check_success(int) -> conjunction<
                std::integral_constant<bool, noexcept(std::declval<F_&>()(std::declval<As>()...))>,
                is_result_t<decltype(std::declval<F_&>()(std::declval<As>()...))>>;

            template <typename...>
            static auto check_fail(...) -> std::false_type;

            template <typename G_>
            static auto check_fail(int) -> conjunction<
                std::integral_constant<bool, noexcept(std::declval<G_&>()(std::declval<flow_async_agg_err_t>()))>,
                is_result_t<decltype(std::declval<G_&>()(std::declval<flow_async_agg_err_t>()))>>;

            template <typename...>
            static auto check_return_match(...) -> std::false_type;

            template <typename F_, typename G_, typename... As>
            static auto check_return_match(int) -> std::is_same<
                decltype(std::declval<F_&>()(std::declval<As>()...)),
                decltype(std::declval<G_&>()(std::declval<flow_async_agg_err_t>()))
            >;

            static constexpr bool value = conjunction_v<
                decltype(check_success<F, Args_...>(0)),
                decltype(check_fail<G>(0)),
                decltype(check_return_match<F, G, Args_...>(0))>;
        };

        template <typename F, typename G, typename... Args_>
        struct check_when_any_success_compatibility {
            template <typename...>
            static auto check_success(...) -> std::false_type;

            template <typename F_, typename... As>
            static auto check_success(int) -> conjunction<
                std::integral_constant<bool, noexcept(std::declval<F_&>()(std::declval<As>()))>...>;

            template <typename...>
            static auto check_fail(...) -> std::false_type;

            template <typename G_>
            static auto check_fail(int) -> conjunction<
                std::integral_constant<bool, noexcept(std::declval<G_&>()(std::declval<flow_async_agg_err_t>()))>,
                is_result_t<decltype(std::declval<G_&>()(std::declval<flow_async_agg_err_t>()))>>;

            template <typename...>
            static auto check_all_returns(...) -> std::false_type;

            template <typename F_, typename G_, typename... As>
            static auto check_all_returns(int) -> conjunction<
                is_result_t<decltype(std::declval<G_&>()(std::declval<flow_async_agg_err_t>()))>,
                is_all_the_same<
                    decltype(std::declval<G_&>()(std::declval<flow_async_agg_err_t>())),
                    decltype(std::declval<F_&>()(std::declval<As>()))...
                >>;

            static constexpr bool value = conjunction_v<
                decltype(check_success<F, Args_...>(0)),
                decltype(check_fail<G>(0)),
                decltype(check_all_returns<F, G, Args_...>(0))
            >;
        };

        template <typename F, typename Pack, size_t ... idx>
        auto unpack_and_call_impl(F&& f, Pack&& pack, std::index_sequence<idx...>) noexcept {
            return std::forward<F>(f)(get<idx>(std::forward<Pack>(pack)).value()...);
        }

        template <typename F, typename ... Ts>
        auto unpack_and_call(F& f, flat_storage<Ts...>&& pack) noexcept {
            return unpack_and_call_impl(f, std::move(pack), std::index_sequence_for<Ts...> {});
        }

        template <typename F, typename Pack>
        struct visit_jump_table_helper {
            using R = invoke_result_t<F, typename flat_storage_element_t<0, Pack>::value_type>;

            template <size_t I>
            static R call(F& f, Pack&& pack) noexcept {
                return f(get<I>(std::forward<Pack>(pack)).value());
            }
        };

        template <typename F, typename Pack, size_t... idx>
        auto visit_and_call_jump_table_impl(F&& f, Pack&& pack, size_t i, std::index_sequence<idx...>) noexcept {
            using Helper = visit_jump_table_helper<F, Pack>;
            using R = typename Helper::R;
            using FuncPtr = R (*)(F&, Pack&&);

            static constexpr FuncPtr table[] = {
                &Helper::template call<idx>...
            };

            if (i >= sizeof...(idx)) {
                return R(error_tag, typename R::error_type {});
            }

            return table[i](f, std::forward<Pack>(pack));
        }

       template <typename F, typename... Ts>
       auto visit_and_call(F& f, size_t i, flat_storage<Ts...>&& pack) noexcept {
           return visit_and_call_jump_table_impl(f, std::move(pack), i, std::index_sequence_for<Ts...> {});
       } 

        template <typename F_O, typename E, typename F, typename F_I>
        result_t<F_O, E> call(std::false_type, std::false_type, F& f, F_I&& in) 
            noexcept(is_nothrow_invocable_with<F&, F_I&&>::value
            && std::is_nothrow_constructible<
                result_t<F_O, E>,
                decltype(value_tag),
                invoke_result_t<F&, F_I&&>>::value) {
            return result_t<F_O, E>(value_tag, f(std::forward<F_I>(in)));
        }

        template <typename F_O, typename E, typename F, typename F_I>
        result_t<F_O, E> call(std::true_type, std::false_type, F& f, F_I&& in) 
            noexcept(is_nothrow_invocable_with<F&, F_I&&>::value) {
            f(std::forward<F_I>(in));
            return result_t<F_O, E>(value_tag);
        }

        template <typename F_O, typename E, typename F, typename F_I>
        result_t<F_O, E> call(std::false_type, std::true_type, F& f, F_I&&) 
            noexcept(conjunction_v<is_nothrow_invocable_with<F&, void>,
                std::is_nothrow_constructible<result_t<F_O, E>, decltype(value_tag), invoke_result_t<F&>>>) {
            return result_t<F_O, E>(value_tag, f());
        }

        template <typename F_O, typename E, typename F, typename F_I>
        result_t<F_O, E> call(std::true_type, std::true_type, F& f, F_I&&) 
            noexcept(is_nothrow_invocable_with<F&, void>::value) {
            f();
            return result_t<F_O, E>(value_tag);
        }

        // transform
        template <typename F>
        struct transform_node {
            F f;

            template <typename F_I, typename E, 
                std::enable_if_t<negation_v<std::is_void<F_I>>>* = nullptr>
            static auto make(transform_node&& self) noexcept(std::is_nothrow_move_constructible<F>::value) {
                using F_O = invoke_result_t<F, F_I>;
                auto wrapper = [f = std::move(self.f)](result_t<F_I, E>&& in) mutable
                    noexcept(noexcept(call<F_O, E, F>(std::is_void<F_O> {}, std::false_type{}, std::declval<F&>(), std::declval<F_I>()))) {
                    LIKELY_IF (in.has_value()) {
                        return call<F_O, E, F>(std::is_void<F_O> {}, std::false_type{}, f, std::move(in).value());
                    }
                    return result_t<F_O, E>(error_tag, std::move(in).error());
                };
                return flow_calc_node<result_t<F_I, E>, result_t<F_O, E>, decltype(wrapper)>(std::move(wrapper));
            }

            template <typename F_I, typename E, 
                std::enable_if_t<std::is_void<F_I>::value>* = nullptr>
            static auto make(transform_node&& self) noexcept(std::is_nothrow_move_constructible<F>::value) {
                using F_O = invoke_result_t<F>;
                auto wrapper = [f = std::move(self.f)](result_t<F_I, E>&& in) mutable
                        noexcept(noexcept(call<F_O, E, F>(std::is_void<F_O> {}, std::true_type{}, std::declval<F&>(),
                            std::declval<result_t<F_I, E>>()))) {
                    LIKELY_IF (in.has_value()) {
                        return call<F_O, E, F>(std::is_void<F_O> {}, std::true_type{}, f, std::move(in));
                    }
                    return result_t<F_O, E>(error_tag, std::move(in).error());
                };
                return flow_calc_node<result_t<F_I, E>, result_t<F_O, E>, decltype(wrapper)>(std::move(wrapper));
            }
        };

        template <typename I, typename O, typename... Nodes, typename F>
        auto operator|(flow_blueprint<I, O, Nodes...>&& bp, transform_node<F>&& a) {
            static_assert(is_nothrow_invocable_with<F, typename O::value_type>::value,
                "The callable F is not compatible with current blueprint, "
                "and must be nothrow-invocable.");

            using T = typename O::value_type;
            using E = typename O::error_type;
            auto node = transform_node<F>::template make<T, E>(std::move(a));

            return std::move(bp) | std::move(node);
        }

        // then
        template <typename F>
        struct then_node {
            F f;

            template <typename F_I, typename F_O>
            static auto make(then_node&& self) 
                noexcept(std::is_nothrow_move_constructible<F>::value) {
                auto wrapper = [f = std::move(self.f)](F_I&& in) noexcept {
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
                    try {
#endif
                        LIKELY_IF(in.has_value()) {
                            return f(std::move(in));
                        }
                        return F_O(error_tag, std::move(in).error());
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
                    } catch (...) {
                        return F_O(error_tag, std::current_exception());
                    }
#endif
                };
                return flow_calc_node<F_I, F_O, decltype(wrapper)>(std::move(wrapper));
            }
        };

        template <typename I, typename O, typename... Nodes, typename F>
        auto operator|(flow_blueprint<I, O, Nodes...>&& bp, then_node<F>&& a) {
#if FLUEX_FOUNDRY_HAS_EXCEPTIONS
            static_assert(is_invocable_with<F, O&&>::value,
                "callable F is not compatible with current blueprint");
#else
            static_assert(is_nothrow_invocable_with<F, O>::value,
                "callable F is not compatible with current blueprint"
                "and must be nothrow-invocable.");
#endif
            using F_O = invoke_result_t<F, O&&>;
            static_assert(is_result_t<F_O>::value,
                "the output of the callable F in then must return a result<T, E>");

            auto node = then_node<F>::template make<O, F_O>(std::move(a));
            return std::move(bp) | std::move(node);
        }

        // error_recover
        template <typename F>
        struct error_node {
            F f;

            template <typename F_I, typename F_O>
            static auto make(error_node&& self) 
                    noexcept(std::is_nothrow_move_constructible<F>::value) {
                auto wrapper = [f = std::move(self.f)](F_I&& in) noexcept {
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
                    try {
#endif
                        LIKELY_IF(in.has_value()) {
                            return F_O(value_tag, std::move(in).value());
                        }
                        return f(std::move(in));
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
                    } catch (...) {
                        return F_O(error_tag, std::current_exception());
                    }
#endif
                };
                return flow_calc_node<F_I, F_O, decltype(wrapper)>(std::move(wrapper));
            }
        };

        template <typename I, typename O, typename ... Nodes, typename F>
        auto operator|(flow_blueprint<I, O, Nodes ...>&& bp, error_node<F>&& a) {
#if FLUEX_FOUNDRY_HAS_EXCEPTIONS
            static_assert(is_invocable_with<F, O&&>::value,
                "The callable F in error is not compatible with current blueprint.");
#else
            static_assert(is_nothrow_invocable_with<F, O>::value,
                "The callable F in error is not compatible with current blueprint."
                "and must be nothrow-invocable.");
#endif

            using F_O = invoke_result_t<F, O&&>;
            static_assert(is_result_t<F_O>::value, "The callable F in error must return a result<T, E>");

            auto node = error_node<F>::template make<O, F_O>(std::move(a));
            return std::move(bp) | std::move(node);
        }

#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        // exception catch
        template <typename F, typename Exception>
        struct exception_catch_node {
            F f;

            template <typename F_I, typename F_O>
            static auto make(exception_catch_node&& self) 
                noexcept(std::is_nothrow_move_constructible<F>::value) {
                using R = result_t<F_O, std::exception_ptr>;
                auto wrapper = [f = std::move(self.f)](F_I&& in) mutable noexcept {
                    LIKELY_IF (in.has_value()) {
                        return R(value_tag, std::move(in).value());
                    }

                    try {
                        std::rethrow_exception(in.error());
                    } catch (const Exception& e) {
                        try {
                            return call<F_O, std::exception_ptr, F>(std::is_void<F_O>{}, std::false_type{}, f, e);
                        } catch (...) {
                            return R(error_tag, std::current_exception());
                        }
                    } catch (...) {
                        return R(error_tag, std::current_exception());
                    }
                };
                return flow_calc_node<F_I, R, decltype(wrapper)>(std::move(wrapper));
            }
        };

        template <typename I, typename O, typename ... Nodes, typename F, typename Exception>
        auto operator|(flow_blueprint<I, O, Nodes ...>&& bp, exception_catch_node<F, Exception>&& a) {
            static_assert(std::is_base_of<std::exception, Exception>::value,
                "The callable F must take a class inherits std::exception.");

            using T = typename O::value_type;
            using F_O = invoke_result_t<F, Exception>;
            static_assert(std::is_convertible<T, F_O>::value, "The callable F in catch_exception must return a value "
                                                              "which is convertible the last node's value type,"
                                                              "namely typename result<T, E>::value_type");
            using E = typename O::error_type;
            static_assert(std::is_convertible<E, std::exception_ptr>::value,
                "catch_exception requires the error_type of the current blueprint "
                "to be std::exception_ptr (or convertible to it).");

            auto node = exception_catch_node<F, Exception>::template make<O, F_O>(std::move(a));
            return std::move(bp) | std::move(node);
        }
#endif
        // via
        template <typename Executor>
        struct via_node {
            static_assert(check_executor<Executor>::value,
                "Executor must be pointer-like and support "
                "noexcept exec->dispatch(task_wrapper_sbo)."
                " Besides, please never ever use inline executor to dispatch await operation");

            Executor e;

            template <typename F_I>
            static auto make(via_node&& node) noexcept {
                auto wrapper = [e = std::move(node.e)](task_wrapper_sbo&& sbo) noexcept {
                    e->dispatch(std::move(sbo));
                };
                return flow_via_node<F_I, F_I, decltype(wrapper)>(std::move(wrapper));
            }
        };

        template <typename I, typename O, typename... Nodes, typename Executor>
        auto operator|(flow_blueprint<I, O, Nodes...>&& bp, via_node<Executor>&& a) {
            auto node = via_node<Executor>::template make<O>(std::move(a));
            return std::move(bp) | std::move(node);
        }

        // async
        template <typename Executor, typename Awaitable>
        struct async_node {
            static_assert(check_executor<Executor>::value,
                "Executor must be pointer-like and support "
                "noexcept exec->dispatch(task_wrapper_sbo)."
                " Besides, please never ever use inline executor to dispatch await operation");

            static_assert(is_awaitable_v<Awaitable>,
                "Awaitable must be an valid awaitable(see flux_foundry::awaitable_base)");
            Executor e;

            template <typename F_I, typename F_O>
            static auto make(async_node&& node) noexcept {
                auto wrapper = [e = std::move(node.e)](task_wrapper_sbo&& sbo) noexcept {
                    e->dispatch(std::move(sbo));
                };

                return flow_async_node<F_I, F_O, decltype(wrapper), flow_impl::identity, awaitable_factory<Awaitable>> {
                    std::move(wrapper), identity{}, awaitable_factory<Awaitable>{}
                };
            }
        };

        template <typename I, typename O, typename... Nodes, typename Executor, typename Awaitable>
        auto operator|(flow_blueprint<I, O, Nodes...>&& bp, async_node<Executor, Awaitable>&& a) {
            using F_O = typename Awaitable::async_result_type;

            static_assert(is_result_t_v<F_O>,
                "Awaitable must provide a result_t<T, E> as it's async result");

            static_assert(std::is_constructible<Awaitable, O&&>::value,
                "awaitable must could be constructible with the current output.");

            return std::move(bp) | async_node<Executor, Awaitable>::template make<O, F_O>(std::move(a));
        }

        // when_all_node
        template <typename Executor, typename F, typename G, typename ... BPs>
        struct when_all_node {
            static_assert(conjunction_v<is_runnable_bp<BPs>...>, "BPs should be runnable_bps");

            static_assert(check_when_all_success_compatibility<F, G, typename BPs::O_t::value_type...>::value,
                "the success proc must have the signature like\n"
                "result_t<T, E> (output_of_bp1, output_of_bp2, output_of_bp3...) noexcept\n"
                "in addition, the fail proc must be compatible should have the signature like\n"
                "result_t<T, E> (flow_async_agg_err_t) noexcept \n"
                "and the success proc and the fail proc should have the same return type");

            Executor e;
            F f;
            G g;

            using F_O = decltype(std::declval<G&>()(std::declval<flow_async_agg_err_t>()));
            using awaitable_t = flow_when_all_awaitable<BPs...>;

            template <typename F_I>
            static auto make(when_all_node&& node, lite_ptr<BPs> ... bps) noexcept {
                auto wrapper = [e = std::move(node.e)](task_wrapper_sbo&& sbo) noexcept {
                    e->dispatch(std::move(sbo));
                };

                using result_type = typename awaitable_t::result_type;
                using factory_t = aggregator_awaitable_factory<awaitable_t, BPs...>;
                auto adaptor = [f = std::move(node.f), g = std::move(node.g)](result_type&& t) noexcept {
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
                    try {
#endif
                        LIKELY_IF (t.has_value()) {
                            return unpack_and_call(f, std::move(t.value().get()));
                        }
                        return g(std::move(t.error()));
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
                    } catch (...) {
                        return F_O(error_tag, std::current_exception());
                    }
#endif
                };

                return flow_async_node<F_I, F_O, decltype(wrapper), decltype(adaptor), factory_t> {
                    std::move(wrapper), std::move(adaptor), factory_t(std::move(bps)...)
                };
            }
        };

        // when_any_node
        template <typename Executor, typename F, typename G, typename ... BPs>
        struct when_any_node {
            static_assert(conjunction_v<is_runnable_bp<BPs>...>, "BPs should be runnable_bps");

            static_assert(check_when_any_success_compatibility<F, G, typename BPs::O_t::value_type...>::value,
                "the success proc must can be called by\n"
                "result_t<T, E> (put_of_bp1) noexcept, result_t<T, E> (put_of_bp2) noexcept ...\n"
                "in addition, the fail proc must be compatible should have the signature like\n"
                "result_t<T, E> (flow_async_agg_err_t) noexcept \n"
                "and the success proc and the fail proc should have the same return type");

            Executor e;
            F f;
            G g;

            using F_O = decltype(std::declval<G&>()(std::declval<flow_async_agg_err_t>()));
            using awaitable_t = flow_when_any_awaitable<BPs...>;

            template <typename F_I>
            static auto make(when_any_node&& node, lite_ptr<BPs> ... bps) noexcept {
                auto wrapper = [e = std::move(node.e)](task_wrapper_sbo&& sbo) noexcept {
                    e->dispatch(std::move(sbo));
                };

                using result_type = typename awaitable_t::result_type;
                using factory_t = aggregator_awaitable_factory<awaitable_t, BPs...>;

                auto adaptor = [f = std::move(node.f), g = std::move(node.g)](result_type&& t) noexcept {
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
                    try {
#endif
                        LIKELY_IF (t.has_value()) {
                            auto winner = t.value().winner();
                            return visit_and_call(f, winner, std::move(t.value().get()));
                        }
                        return g(std::move(t.error()));
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
                    } catch (...) {
                        return F_O(error_tag, std::current_exception());
                    }
#endif
                };

                return flow_async_node<F_I, F_O, decltype(wrapper), decltype(adaptor), factory_t> {
                    std::move(wrapper), std::move(adaptor), factory_t(std::move(bps)...)
                };
            }
        };

        // end
        template <typename F>
        struct end_node {
            F f;

            template <typename F_I>
            static auto make(end_node&& self)
                noexcept(std::is_nothrow_move_constructible<F>::value) {
                auto wrapper = [f = std::move(self.f)](F_I&& in) noexcept {
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
                    try {
#endif
                        return f(std::move(in));
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
                    } catch (...) {
                        return F_I(error_tag, std::current_exception());
                    }
#endif
                };
                return flow_end_node<F_I, F_I, decltype(wrapper)>(std::move(wrapper));
            }
        };

        template <>
        struct end_node <void> {
            template <typename F_I>
            static auto make(end_node&&) noexcept {
                auto wrapper = [](F_I&& in) noexcept {
                    return in;
                };
                return flow_end_node<F_I, F_I, decltype(wrapper)>(std::move(wrapper));
            }
        };

        template <typename I, typename O, typename... Nodes,
            typename F, std::enable_if_t<!std::is_void<F>::value, int> = 0>
        auto operator|(flow_blueprint<I, O, Nodes...>&& bp, end_node<F>&& a) {

#if FLUEX_FOUNDRY_HAS_EXCEPTIONS
            static_assert(is_invocable_with<F, O&&>::value,
                "The callable F in end is not compatible with current blueprint.");
#else
            static_assert(is_nothrow_invocable_with<F, O>::value,
                "The callable F in end is not compatible with current blueprint."
                "and must be nothrow-invocable.");
#endif

            using F_O = invoke_result_t<F, O&&>;
            static_assert(std::is_same<O, F_O>::value,
                "The callable F in end must return a object of the same type as the current Blueprint's output");

            auto node = end_node<F>::template make<O>(std::move(a));
            return std::move(bp) | std::move(node);
        }

        template <typename I, typename O, typename... Nodes,
            typename F, std::enable_if_t<std::is_void<F>::value, int> = 0>
        auto operator|(flow_blueprint<I, O, Nodes...>&& bp, end_node<F>&& a) {
            auto node = end_node<void>::make<O>(std::move(a));
            return std::move(bp) | std::move(node);
        }
    }

    template <typename T, typename E = std::exception_ptr>
    auto make_blueprint() noexcept(conjunction_v<
        std::is_nothrow_move_constructible<T>,
        std::is_nothrow_constructible<result_t<T, E>, decltype(value_tag), T&&>>) {
        using R = result_t<T, E>;

        using node_type = flow_impl::flow_calc_node<R, R, flow_impl::identity>;
        using storage_t = flat_storage<node_type>;

        return flow_impl::flow_blueprint<R, R, node_type>(
            storage_t(node_type(flow_impl::identity{}))
        );
    }

    template <typename F>
    auto transform(F&& f) noexcept {
        return flow_impl::transform_node<std::decay_t<F>> { std::forward<F>(f) };
    }

    template <typename F>
    auto then(F&& f) noexcept {
        return flow_impl::then_node<std::decay_t<F>> { std::forward<F>(f) };
    }

    template <typename F>
    auto on_error(F&& f) noexcept {
        return flow_impl::error_node<std::decay_t<F>> { std::forward<F>(f) };
    }

#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    template <typename Exception, typename F>
    inline auto catch_exception(F&& f) noexcept {
        return flow_impl::exception_catch_node<std::decay_t<F>, Exception> { std::forward<F>(f) };
    }
#endif

    // CRITICAL: Max payload size is controlled by the SBO buffer (e.g., 64 bytes).
    // Ensure that the captured data (result_t) does not exceed the remaining buffer space.(OR it will trigger heap alloc)
    template <typename Executor>
    auto via(Executor&& exec) noexcept {
        using E = std::decay_t<Executor>;
        return flow_impl::via_node<E> { std::forward<Executor>(exec) };
    }

    // CRITICAL: Max payload size is controlled by the SBO buffer (e.g., 64 bytes).
    // Ensure that the async result(result_t) does not exceed the remaining buffer space.(OR it will trigger heap alloc)
    template <typename Awaitable, typename Executor>
    auto await(Executor&& executor_to_resume) noexcept {
        using E = std::decay_t<Executor>;
        return flow_impl::async_node<E, Awaitable> { std::forward<Executor>(executor_to_resume) };
    }

    template <typename Executor, typename F, typename G, typename ... BPs>
    auto await_when_all(Executor&& executor_to_resume, F&& on_success, G&& on_error, lite_ptr<BPs> ... bps) noexcept {
        using E = std::decay_t<Executor>;

        using when_all_t = flow_impl::when_all_node<E, std::decay_t<F>, std::decay_t<G>, BPs...>;
        using F_I = result_t<flat_storage<typename BPs::I_t::value_type...>, flow_async_agg_err_t>;
        using F_O = typename when_all_t::F_O;

        when_all_t when_all{std::forward<Executor>(executor_to_resume),
            std::forward<F>(on_success), std::forward<G>(on_error)};

        auto node = when_all_t::template make<F_I>(std::move(when_all), std::move(bps)...);
        return flow_impl::flow_blueprint<F_I, F_O, decltype(node)>(flat_storage<decltype(node)>(std::move(node)));
    }

    template <typename Executor, typename F, typename G, typename ... BPs>
    auto await_when_any(Executor&& executor_to_resume, F&& on_success, G&& on_error, lite_ptr<BPs> ... bps) noexcept {
        using E = std::decay_t<Executor>;

        using when_any_t = flow_impl::when_any_node<E, std::decay_t<F>, std::decay_t<G>, BPs...>;
        using F_I = result_t<flat_storage<typename BPs::I_t::value_type...>, flow_async_agg_err_t>;
        using F_O = typename when_any_t::F_O;

        when_any_t when_any{std::forward<Executor>(executor_to_resume), std::forward<F>(on_success), std::forward<G>(on_error)};
        auto node = when_any_t::template make<F_I>(std::move(when_any), std::move(bps)...);
        return flow_impl::flow_blueprint<F_I, F_O, decltype(node)>(flat_storage<decltype(node)>(std::move(node)));
    }

    template <typename F>
    auto end(F&& f) noexcept {
        return flow_impl::end_node<std::decay_t<F>> { std::forward<F>(f) };
    }

    inline auto end() noexcept {
        return flow_impl::end_node<void>{};
    }
}

#endif
