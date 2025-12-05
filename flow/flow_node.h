#ifndef LITE_FNDS_FLOW_NODES_H
#define LITE_FNDS_FLOW_NODES_H

#include "../task/task_wrapper.h"
#include "flow_blueprint.h"

namespace lite_fnds {
    namespace flow_impl {
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

        /// transform
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
        auto operator|(flow_blueprint<I, O, Nodes...> bp, transform_node<F> a) {
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
#if LFNDS_COMPILER_HAS_EXCEPTIONS
                    try {
#endif
                        LIKELY_IF(in.has_value())
                        {
                            return f(std::move(in));
                        }
                        return F_O(error_tag, std::move(in).error());
#if LFNDS_COMPILER_HAS_EXCEPTIONS
                    } catch (...) {
                        return F_O(error_tag, std::current_exception());
                    }
#endif
                };
                return flow_calc_node<F_I, F_O, decltype(wrapper)>(std::move(wrapper));
            }
        };

        template <typename I, typename O, typename... Nodes, typename F>
        auto operator|(flow_blueprint<I, O, Nodes...> bp, then_node<F> a) {
#if LFNDS_HAS_EXCEPTIONS
            static_assert(is_invocable_with<F, O>::value,
                "callable F is not compatible with current blueprint");
#else
            static_assert(is_nothrow_invocable_with<F, O>::value,
                "callable F is not compatible with current blueprint"
                "and must be nothrow-invocable.");
#endif
            using F_O = invoke_result_t<F, O>;
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
#if LFNDS_COMPILER_HAS_EXCEPTIONS
                    try {
#endif
                        LIKELY_IF(in.has_value())
                        {
                            return F_O(value_tag, std::move(in).value());
                        }
                        return f(std::move(in));
#if LFNDS_COMPILER_HAS_EXCEPTIONS
                    } catch (...) {
                        return F_O(error_tag, std::current_exception());
                    }
#endif
                };
                return flow_calc_node<F_I, F_O, decltype(wrapper)>(std::move(wrapper));
            }
        };

        template <typename I, typename O, typename ... Nodes, typename F>
        auto operator|(flow_blueprint<I, O, Nodes ...> bp, error_node<F> a) {
#if LFNDS_HAS_EXCEPTIONS
            static_assert(is_invocable_with<F, O>::value,
                "The callable F in error is not compatible with current blueprint.");
#else
            static_assert(is_nothrow_invocable_with<F, O>::value,
                "The callable F in error is not compatible with current blueprint."
                "and must be nothrow-invocable.");
#endif

            using F_O = invoke_result_t<F, O>;
            static_assert(is_result_t<F_O>::value, "The callable F in error must return a result<T, E>");

            auto node = error_node<F>::template make<O, F_O>(std::move(a));
            return std::move(bp) | std::move(node);
        }

#if LFNDS_COMPILER_HAS_EXCEPTIONS
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
        auto operator|(flow_blueprint<I, O, Nodes ...> bp, exception_catch_node<F, Exception> a) {
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
            template <typename X>
            struct check {
                template <typename U>
                static auto detect(int) -> std::integral_constant<bool,
                    noexcept(std::declval<U&>()->dispatch(std::declval<task_wrapper_sbo>()))>;

                template <typename...>
                static auto detect(...) -> std::false_type;

                static constexpr bool value = decltype(detect<X>(0))::value;
            };

            static_assert(check<Executor>::value,
                "Executor must be pointer-like and support "
                "noexcept exec->dispatch(task_wrapper_sbo).");

            Executor e;

            template <typename F_I>
            static auto make(via_node&& node) noexcept {
                auto wrapper = [e = std::move(node.e)](task_wrapper_sbo&& sbo) noexcept {
                    e->dispatch(std::move(sbo));
                };
                return flow_control_node<F_I, F_I, decltype(wrapper)>(std::move(wrapper));
            }
        };

        template <typename I, typename O, typename... Nodes, typename Executor>
        auto operator|(flow_blueprint<I, O, Nodes...> bp, via_node<Executor> a) {
            auto node = via_node<Executor>::template make<O>(std::move(a));
            return std::move(bp) | std::move(node);
        }

        // end
        template <typename F>
        struct end_node {
            F f;

            template <typename F_I, typename F_O>
            static auto make(end_node&& self)
                noexcept(std::is_nothrow_move_constructible<F>::value) {
                auto wrapper = [f = std::move(self.f)](F_I&& in) noexcept {
#if LFNDS_COMPILER_HAS_EXCEPTIONS
                    try {
#endif
                        return f(std::move(in));
#if LFNDS_COMPILER_HAS_EXCEPTIONS
                    } catch (...) {
                        return F_O(error_tag, std::current_exception());
                    }
#endif
                };
                return flow_end_node<F_I, F_O, decltype(wrapper)>(std::move(wrapper));
            }
        };

        template <>
        struct end_node <void> {
            template <typename F_I, typename F_O>
            static auto make(end_node&&) noexcept {
                auto wrapper = [](F_I&& in) noexcept {
                    return in;
                };
                return flow_end_node<F_I, F_O, decltype(wrapper)>(std::move(wrapper));
            }
        };

        template <typename I, typename O, typename... Nodes,
            typename F, std::enable_if_t<!std::is_void<F>::value, int> = 0>
        auto operator|(flow_blueprint<I, O, Nodes...> bp, end_node<F> a) {

#if LFNDS_HAS_EXCEPTIONS
            static_assert(is_invocable_with<F, O>::value,
                "The callable F in end is not compatible with current blueprint.");
#else
            static_assert(is_nothrow_invocable_with<F, O>::value,
                "The callable F in end is not compatible with current blueprint."
                "and must be nothrow-invocable.");
#endif

            using F_O = invoke_result_t<F, O>;
            static_assert(is_result_t<F_O>::value, "The callable F in end must return a result<T, E>-like type");

            auto node = end_node<F>::template make<O, F_O>(std::move(a));
            return std::move(bp) | std::move(node);
        }

        template <typename I, typename O, typename... Nodes,
            typename F, std::enable_if_t<std::is_void<F>::value, int> = 0>
        auto operator|(flow_blueprint<I, O, Nodes...> bp, end_node<F> a) {
            auto node = end_node<void>::make<O, O>(std::move(a));
            return std::move(bp) | std::move(node);
        }
    }

    template <typename T, typename E = std::exception_ptr>
    inline auto make_blueprint() noexcept(conjunction_v<
        std::is_nothrow_move_constructible<T>,
        std::is_nothrow_constructible<result_t<T, E>, decltype(value_tag), T&&>>) {
        using R = result_t<T, E>;

        auto identity = [](R t) noexcept {
            return t;
        };

        using node_type = flow_impl::flow_calc_node<R, R, decltype(identity)>;
        using storage_t = std::tuple<node_type>;

        return flow_impl::flow_blueprint<R, R, node_type>(
            storage_t(node_type(std::move(identity)))
        );
    }


    template <typename F>
    inline auto transform(F&& f) noexcept {
        return flow_impl::transform_node<std::decay_t<F>> { std::forward<F>(f) };
    }

    template <typename F>
    inline auto then(F&& f) noexcept {
        return flow_impl::then_node<std::decay_t<F>> { std::forward<F>(f) };
    }

    template <typename F>
    inline auto on_error(F&& f) noexcept {
        return flow_impl::error_node<std::decay_t<F>> { std::forward<F>(f) };
    }

#if LFNDS_COMPILER_HAS_EXCEPTIONS
    template <typename Exception, typename F>
    inline auto catch_exception(F&& f) noexcept {
        return flow_impl::exception_catch_node<std::decay_t<F>, Exception> { std::forward<F>(f) };
    }
#endif

    // CRITICAL: Max payload size is controlled by the SBO buffer (e.g., 64 bytes).
    // Ensure that the captured data (result_t) does not exceed the remaining buffer space.(OR it will trigger heap alloc)
    template <typename Executor>
    inline auto via(Executor&& exec) noexcept {
        using E = std::decay_t<Executor>;
        return flow_impl::via_node<E> { std::forward<Executor>(exec) };
    }

    template <typename F>
    inline auto end(F&& f) noexcept {
        return flow_impl::end_node<std::decay_t<F>> { std::forward<F>(f) };
    }

    inline auto end() noexcept {
        return flow_impl::end_node<void>{};
    }
}

#endif
