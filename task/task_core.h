#ifndef LITE_FNDS_TASK_CORE_H
#define LITE_FNDS_TASK_CORE_H

#include <type_traits>
#include <utility>
#include <cassert>
#include <memory>

#include "../base/traits.h"
#include "../memory/result_t.h"
#include "../memory/flat_storage.h"

namespace lite_fnds {
    namespace task_impl_private {
        enum data_index {
            data_callable,
            data_params,
            data_object,
        };

        // NOTE: If R is already a result_t<...>, we assume its error type is std::exception_ptr.
        template <typename R>
        struct uniform_result :
            std::conditional_t<is_result_t<R>::value,
                type_identity<R>,
                type_identity<result_t<R, std::exception_ptr>>> {};

        template <typename R>
        using uniform_result_t = typename uniform_result<R>::type;

        template <typename R, typename V,
                  std::enable_if_t<!is_result_t<R>::value>* = nullptr>
        uniform_result_t<R> make_success_result(V&& v) {
            return uniform_result_t<R>(value_tag, std::forward<V>(v));
        }

        template <typename R, typename V,
                  std::enable_if_t<is_result_t<R>::value>* = nullptr>
        uniform_result_t<R> make_success_result(V&& v) {
            return std::forward<V>(v);
        }

        template <typename R>
        uniform_result_t<R> make_error_result(std::exception_ptr&& e) {
            using Res = uniform_result_t<R>;
            return Res(error_tag, std::move(e));
        }

        template<typename T>
        decltype(auto) unpack_param(std::reference_wrapper<T> r) noexcept {
            return r.get();
        }

        template<typename T>
        decltype(auto) unpack_param(T &x) noexcept {
            return std::move(x);
        }

        template<bool IsMemberFunction, typename Callable, typename R, typename... Args>
        class task_impl;

        template <typename Callable, typename object, typename R, typename... Args>
        class task_impl<true, Callable, object, R, Args...> {
#if !LFNDS_HAS_EXCEPTIONS
            static_assert(is_result_t<R>::value, "The invoke result of Callable with the given params must be a result_t.");
#endif
            using obj_param_t = std::remove_reference_t<object>;

        public:
            using callable_result_t = R;
            using object_storage = std::conditional_t<
                disjunction_v<is_shared_ptr<obj_param_t>, std::is_pointer<obj_param_t> >,
                obj_param_t, std::add_pointer_t<obj_param_t> >;
            using param_type = flat_storage<std::decay_t<Args>...>;

            using result_type = uniform_result_t<R>;
        private:
            using callable_t = std::decay_t<Callable>;
            using data_type = flat_storage<callable_t, param_type, object_storage>;
            data_type _data;

        public:
            // not copyable or copy assignable
            task_impl(const task_impl &) = delete;

            task_impl &operator=(const task_impl &) = delete;

            task_impl(task_impl &&) noexcept(conjunction_v<
                std::is_nothrow_move_constructible<callable_t>,
                std::is_nothrow_move_constructible<param_type>,
                std::is_nothrow_move_constructible<object_storage> >) = default;

            task_impl &operator=(task_impl &&) noexcept(conjunction_v<
                std::is_nothrow_move_assignable<callable_t>,
                std::is_nothrow_move_assignable<param_type>,
                std::is_nothrow_move_assignable<object_storage> >) = default;

            ~task_impl() = default;

            task_impl(Callable, std::remove_pointer_t<obj_param_t> &&, Args &&...) = delete;

            template <typename ... P, typename OP = obj_param_t,
#if LFNDS_HAS_EXCEPTIONS
                std::enable_if_t<conjunction_v<is_shared_ptr<OP>,
                    std::is_move_constructible<callable_t>,
                    std::is_constructible<param_type, P&&...>>>* = nullptr
#else
                std::enable_if_t<conjunction_v<is_shared_ptr<OP>,
                    std::is_nothrow_move_constructible<callable_t>,
                    std::is_nothrow_constructible<param_type, P&&...>>>* = nullptr
#endif
            >
            explicit task_impl(callable_t pmf, OP obj, P&&... args)
                noexcept(conjunction_v<
                    std::is_nothrow_move_constructible<callable_t>,
                    std::is_nothrow_constructible<param_type, P &&...> >)
                : _data{std::move(pmf), param_type{std::forward<P>(args)...}, std::move(obj)} {
                assert(obj && pmf);
            }

            template <typename... P, typename OP = obj_param_t,
#if LFNDS_HAS_EXCEPTIONS
                std::enable_if_t<conjunction_v<negation<is_shared_ptr<OP>>,
                    std::is_move_constructible<callable_t>,
                    std::is_constructible<param_type, P&&...>>>* = nullptr
#else
                std::enable_if_t<conjunction_v<negation<is_shared_ptr<OP>>,
                    std::is_nothrow_move_constructible<callable_t>,
                    std::is_nothrow_constructible<param_type, P&&...>>>* = nullptr
#endif
            >
            explicit task_impl(callable_t pmf, std::remove_pointer_t<obj_param_t>& obj, P&&... args)
                noexcept(conjunction_v<
                    std::is_nothrow_move_constructible<callable_t>,
                    std::is_nothrow_constructible<param_type, P&&...> >) 
                : _data{std::move(pmf), param_type{std::forward<P>(args)...}, std::addressof(obj)} {
                assert(pmf);
            }

            template <typename ... P, typename OP = obj_param_t
#if LFNDS_HAS_EXCEPTIONS
                std::enable_if_t<conjunction_v<negation<is_shared_ptr<OP>>,
                    std::is_move_constructible<callable_t>,
                    std::is_constructible<param_type, P&&...> >>* = nullptr
#else
                std::enable_if_t<conjunction_v<negation<is_shared_ptr<OP>>,
                    std::is_nothrow_move_constructible<callable_t>,
                    std::is_nothrow_constructible<param_type, P&&...> >>* = nullptr
#endif
            >
            explicit task_impl(callable_t pmf, std::remove_pointer_t<obj_param_t> *obj, P &&... args)
                noexcept(conjunction_v<
                    std::is_nothrow_move_constructible<callable_t>,
                    std::is_nothrow_constructible<param_type, P &&...> >) 
                : _data{std::move(pmf), param_type{std::forward<P>(args)...}, obj} {
                assert(obj && pmf);
            }

#if !LFNDS_HAS_EXCEPTIONS
            template <typename D = data_type, std::enable_if_t<is_nothrow_swappable<D>::value>* = nullptr>
#endif
            void swap(task_impl &rhs)
                noexcept(is_nothrow_swappable<data_type>::value) {
                if (this == &rhs) {
                    return;
                }
                using std::swap;
                swap(_data, rhs._data);
            }

            template<typename _param_type = param_type>
            std::enable_if_t<sizeof ...(Args) != 0, _param_type &> get_params() noexcept {
                return get<data_index::data_params>(_data);
            }

            template<typename _param_type = param_type>
            std::enable_if_t<(sizeof...(Args) != 0), const _param_type &> get_params() const noexcept {
                return get<data_index::data_params>(_data);
            }

            result_type operator()() noexcept {
                return do_execute(std::index_sequence_for<Args...>(), std::is_same<R, void>());
            }
        private:
#if LFNDS_HAS_EXCEPTIONS
            template <size_t ... idx>
            result_type do_execute(const std::integer_sequence<size_t, idx...>&, std::true_type) noexcept {
#if LFNDS_COMPILER_HAS_EXCEPTIONS
                try {
#endif
                    auto& obj = get<data_index::data_object>(_data);
                    auto& _callable = get<data_index::data_callable>(_data);
                    auto& params = get<data_index::data_params>(_data);
                    ((*obj).*_callable)(unpack_param(get<idx>(params))...);
                    return result_type(value_tag);
#if LFNDS_COMPILER_HAS_EXCEPTIONS
                } catch (...) {
                    return result_type(error_tag, std::current_exception());
                }
#endif
            }

            template <size_t ... idx>
            result_type do_execute(const std::integer_sequence<size_t, idx...>&, std::false_type) noexcept {
#if LFNDS_COMPILER_HAS_EXCEPTIONS
                try {
#endif
                    auto& obj = get<data_index::data_object>(_data);
                    auto& _callable = get<data_index::data_callable>(_data);
                    auto& params = get<data_index::data_params>(_data);
#if LFNDS_COMPILER_HAS_EXCEPTIONS
                    return make_success_result<R>(((*obj).*_callable)(unpack_param(get<idx>(params))...));
                } catch (...) {
                    return make_error_result<R>(std::current_exception());
                }
#else
                return make_success_result<R>(((*obj).*_callable)(unpack_param(get<idx>(params))...));
#endif
            }
        };

        template<typename Callable, typename R, typename ... Args>
        class task_impl<false, Callable, R, Args...> {
#if !LFNDS_HAS_EXCEPTIONS
            static_assert(is_result_t<R>::value, "The invoke result of Callable with the given params must be a result_t.");
#endif
        public:
            using callable_result_t = R;
            using param_type = flat_storage<std::decay_t<Args>...>;
            using result_type = uniform_result_t<R>;

        private:
            using callable_t = std::decay_t<Callable>;
            using data_type = flat_storage<callable_t, param_type>;
        public:
            // not copyable or copy assignable
            task_impl(const task_impl &) = delete;

            task_impl &operator=(const task_impl &) = delete;

            task_impl(task_impl &&)
                noexcept(conjunction_v<std::is_nothrow_move_constructible<callable_t>,
                    std::is_nothrow_move_constructible<param_type> >) = default;

            task_impl &operator=(task_impl &&)
                noexcept(conjunction_v<std::is_nothrow_move_assignable<callable_t>,
                    std::is_nothrow_move_assignable<param_type> >) = default;

            ~task_impl() = default;

#if !LFNDS_HAS_EXCEPTIONS
            template <typename D = data_type, std::enable_if_t<is_nothrow_swappable<D>::value>* = nullptr>
#endif
            void swap(task_impl &rhs)
                noexcept(is_nothrow_swappable<data_type>::value) {
                if (this != &rhs) {
                    using std::swap;
                    swap(_data, rhs._data);
                }
            }

            template <typename ... P, typename T = callable_t,
#if LFNDS_HAS_EXCEPTIONS
                std::enable_if_t<conjunction_v<
                    std::is_pointer<T>,
                    std::is_move_constructible<T>,
                    std::is_constructible<param_type, P&&...>>>* = nullptr
#else
                std::enable_if_t<conjunction_v<
                    std::is_pointer<T>,
                    std::is_nothrow_move_constructible<T>,
                    std::is_nothrow_constructible<param_type, P &&...>>>* = nullptr
#endif
            >
            explicit task_impl(T func, P &&... args)
                noexcept(conjunction_v<std::is_nothrow_move_constructible<T>, std::is_nothrow_constructible<param_type,
                    P &&...> >)
                : _data{std::move(func), param_type(std::forward<P>(args)...)} {
                assert(func);
            }

            template <typename ... P, typename T = callable_t,
#if LFNDS_HAS_EXCEPTIONS
                std::enable_if_t<conjunction_v<
                    negation<std::is_pointer<T>>,
                    std::is_nothrow_move_constructible<T>,
                    std::is_nothrow_constructible<param_type, P&&...>>>* = nullptr
#else
                std::enable_if_t<conjunction_v<
                    negation<std::is_pointer<T>>,
                    std::is_nothrow_move_constructible<T>,
                    std::is_nothrow_constructible<param_type, P &&...>>>* = nullptr
#endif
                >
            explicit task_impl(T func, P &&... args)
                noexcept(conjunction_v<std::is_nothrow_move_constructible<T>, 
                    std::is_nothrow_constructible<param_type, P &&...> >)
                : _data{std::move(func), param_type(std::forward<P>(args)...)} {
            }

            template<typename _param_type = param_type>
            std::enable_if_t<sizeof...(Args) != 0, _param_type &> get_params() noexcept {
                return get<data_index::data_params>(_data);
            }

            template<typename _param_type = param_type>
            std::enable_if_t<(sizeof...(Args) != 0), const _param_type &> get_params() const noexcept {
                return get<data_index::data_params>(_data);
            }

            result_type operator()() noexcept {
                return do_execute(std::index_sequence_for<Args...>(), std::is_same<R, void>{});
            }
        private:
            template<size_t... idx>
            result_type do_execute(const std::integer_sequence<size_t, idx...>&, std::true_type) noexcept {
#if LFNDS_COMPILER_HAS_EXCEPTIONS
                try {
#endif
                    auto &callable = get<data_index::data_callable>(_data);
                    auto &params = get<data_index::data_params>(_data);
                    callable(unpack_param(get<idx>(params))...);
                    return result_type(value_tag);
#if LFNDS_COMPILER_HAS_EXCEPTIONS
                } catch (...) {
                    return result_type(error_tag, std::current_exception());
                }
#endif
            }

            template <size_t... idx>
            result_type do_execute(const std::integer_sequence<size_t, idx...> &, std::false_type) noexcept {
#if LFNDS_COMPILER_HAS_EXCEPTIONS
                try {
#endif
                    auto &callable = get<data_index::data_callable>(_data);
                    auto &params = get<data_index::data_params>(_data);
#if LFNDS_COMPILER_HAS_EXCEPTIONS
                    return make_success_result<R>(callable(unpack_param(get<idx>(params))...));
                } catch (...) {
                    return make_error_result<R>(std::current_exception());
                }
#else
                return make_success_result<R>(callable(unpack_param(get<idx>(params))...));
#endif
            }

            data_type _data;
        };

        template <typename Obj>
        struct pmf_invoke_obj {
            template<typename T_, bool = is_shared_ptr_v<T_> >
            struct type_getter : type_identity<T_> { };

            template<typename T_>
            struct type_getter<T_, true> : type_identity<typename T_::element_type&> { };

            using raw = std::remove_cv_t<std::remove_reference_t<Obj> >;
            using type = typename std::conditional_t<
                is_shared_ptr<raw>::value,
                type_getter<raw>,
                std::conditional_t<
                    std::is_pointer<raw>::value,
                    type_getter<std::remove_pointer_t<raw> &>, // U* → U&
                    type_getter<raw &> // U → U&
                >
            >::type;
        };

        template<class Obj>
        using pmf_invoke_obj_t = typename pmf_invoke_obj<Obj>::type;

        template<bool IsMemberFunction, typename Callable, typename... Args>
        struct task_selector;

        template<typename Callable, typename... Args>
        struct task_selector<false, Callable, Args...> :
                type_identity<task_impl<false, Callable, invoke_result_t<Callable, Args...>, Args...> > {
        };

        template<typename Callable, typename ObjectType, typename... Args>
        struct task_selector<true, Callable, ObjectType, Args...> :
                type_identity<task_impl<true, Callable, ObjectType,
                    invoke_result_t<Callable, pmf_invoke_obj_t<ObjectType>, Args...>, Args...> > {
        };
    }

    template<typename Callable, typename... Args>
    using task = typename task_impl_private::task_selector<
        std::is_member_function_pointer<std::decay_t<Callable> >::value,
        std::decay_t<Callable>,
        std::decay_t<Args>...>::type;

    template<typename Callable, typename... Args>
    void swap(task<Callable, Args...> &lhs, task<Callable, Args...> &rhs)
        noexcept(noexcept(std::declval<task<Callable, Args...>&>().swap(std::declval<task<Callable, Args...>&>))) {
        lhs.swap(rhs);
    }

    template<class F, class... A>
    auto make_task(F &&f, A &&... a)
        noexcept(std::is_nothrow_constructible<task<std::decay_t<F>, std::decay_t<A>...>, F &&, A &&...>::value)
        -> task<std::decay_t<F>, std::decay_t<A>...> {
        return task<std::decay_t<F>, std::decay_t<A>...>(std::forward<F>(f), std::forward<A>(a)...);
    }

    template<class F, class... A>
    auto make_unique_task(F &&f, A &&... a)
        -> std::unique_ptr<task<std::decay_t<F>, std::decay_t<A>...> > {
        return std::make_unique<task<std::decay_t<F>, std::decay_t<A>...> >(
            std::forward<F>(f), std::forward<A>(a)...);
    }

    template<class F, class... A>
    auto make_shared_task(F &&f, A &&... a)
        -> std::shared_ptr<task<std::decay_t<F>, std::decay_t<A>...> > {
        return std::make_shared<task<std::decay_t<F>, std::decay_t<A>...> >(
            std::forward<F>(f), std::forward<A>(a)...);
    }
}

#endif
