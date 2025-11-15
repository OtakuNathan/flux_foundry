#ifndef __LITE_FNDS_TASK_CORE_H__
#define __LITE_FNDS_TASK_CORE_H__

#include <type_traits>
#include <utility>
#include <tuple>
#include <cassert>
#include <memory>

#include "../base/traits.h"
#include "../memory/result_t.h"

namespace lite_fnds {
    namespace task_impl_private {
        enum data_index {
            data_callable,
            data_params,
            data_object,
        };
    
        template <typename T>
        decltype(auto) unpack_param(std::reference_wrapper<T> r) noexcept {
            return r.get();
        }
    
        template <typename T>
        decltype(auto) unpack_param(T& x) noexcept {
            return std::move(x);
        }
    
        template <bool IsMemberFunction, typename Callable, typename R, typename ... Args>
        class task_impl;
    
        template <typename Callable, typename object, typename R, typename ... Args>
        class task_impl<true, Callable, object, R, Args...> {
            using obj_param_t = std::remove_reference_t<object>;
    
        public:
            using callable_result_t = R;
            using object_storage = std::conditional_t<
                    disjunction_v<is_shared_ptr<obj_param_t>, std::is_pointer<obj_param_t>>,
                    obj_param_t, std::add_pointer_t<obj_param_t>>;
            using param_type = std::tuple<std::decay_t<Args>...>;
            using result_type = result_t<R, std::exception_ptr>;
        private:
            using callable_t = std::decay_t<Callable>;
            using data_type = std::tuple<callable_t, param_type, object_storage>;
            data_type _data;
    
        public:
            // not copyable or copy assignable
            task_impl(const task_impl&) = delete;
            task_impl& operator=(const task_impl&) = delete;
    
            task_impl(task_impl &&) noexcept(conjunction_v<
                std::is_nothrow_move_constructible<callable_t>,
                std::is_nothrow_move_constructible<param_type>,
                std::is_nothrow_move_constructible<object_storage>>) = default;
    
            task_impl &operator=(task_impl &&) noexcept(conjunction_v<
                std::is_nothrow_move_assignable<callable_t>,
                std::is_nothrow_move_assignable<param_type>,
                std::is_nothrow_move_assignable<object_storage>>) = default;
    
            ~task_impl() = default;
    
            task_impl(Callable, std::remove_pointer_t<obj_param_t>&& , Args&&...) = delete;
    
            template <typename OP = obj_param_t, std::enable_if_t<is_shared_ptr<OP>::value>* = nullptr>
            explicit task_impl(callable_t pmf, OP obj, Args&&... args)
                noexcept(conjunction_v<
                    std::is_nothrow_move_constructible<callable_t>,
                    std::is_nothrow_constructible<param_type, Args&&...>>)
                : _data{ std::move(pmf), param_type{std::forward<Args>(args)...}, std::move(obj) } {
                assert(obj && pmf);
            }
    
            template <typename OP = obj_param_t, std::enable_if_t<!is_shared_ptr<OP>::value>* = nullptr>
            explicit task_impl(callable_t pmf, std::remove_pointer_t<obj_param_t>& obj, Args&&... args)
                noexcept(conjunction_v<
                    std::is_nothrow_move_constructible<callable_t>,
                    std::is_nothrow_constructible<param_type, Args&&...>>) :
                _data{ std::move(pmf), param_type{std::forward<Args>(args)...}, std::addressof(obj) } {
                assert(pmf);
            }
    
            template <typename OP = obj_param_t, std::enable_if_t<!is_shared_ptr<OP>::value>* = nullptr>
            explicit task_impl(callable_t pmf, std::remove_pointer_t<obj_param_t>* obj, Args&&... args)
                noexcept(conjunction_v<
                    std::is_nothrow_move_constructible<callable_t>,
                    std::is_nothrow_constructible<param_type, Args&&...>>) :
                _data{ std::move(pmf), param_type{std::forward<Args>(args)...}, obj } {
                assert(obj && pmf);
            }
    
            void swap(task_impl& rhs) noexcept(noexcept(std::swap(_data, rhs._data))) {
                if (this == &rhs) {
                    return;
                }
                using std::swap;
                swap(_data, rhs._data);
            }
    
            template <typename _param_type = param_type>
            std::enable_if_t<sizeof ... (Args) != 0, _param_type&> get_params() noexcept {
                return std::get<data_index::data_params>(_data);
            }
    
            template <typename _param_type = param_type>
            std::enable_if_t<(sizeof...(Args) != 0), const _param_type&> get_params() const noexcept {
                return std::get<data_index::data_params>(_data);
            }
    
            result_type operator()() noexcept {
                return do_execute(std::index_sequence_for<Args...>(), std::is_same<R, void>());
            }
        private:
            template <size_t ... idx>
            result_type do_execute(const std::integer_sequence<size_t, idx...>&, std::true_type) noexcept {
                try {
                    auto& obj = std::get<data_index::data_object>(_data);
                    auto& _callable = std::get<data_index::data_callable>(_data);
                    auto& params = std::get<data_index::data_params>(_data);
                    ((*obj).*_callable)(unpack_param(std::get<idx>(params))...);
                    return result_type(value_tag);
                } catch (...) {
                    return result_type(error_tag, std::current_exception());
                }
            }
    
            template <size_t ... idx>
            result_type do_execute(const std::integer_sequence<size_t, idx...>&, std::false_type) noexcept {
                try {
                    auto& obj = std::get<data_index::data_object>(_data);
                    auto& _callable = std::get<data_index::data_callable>(_data);
                    auto& params = std::get<data_index::data_params>(_data);
                    return result_type(value_tag, ((*obj).*_callable)(unpack_param(std::get<idx>(params))...));
                } catch (...) {
                    return result_type(error_tag, std::current_exception());
                }
            }
        };
    
        template <typename Callable, typename R, typename... Args>
        class task_impl<false, Callable, R, Args...> {
        private:
            using callable_t = std::decay_t<Callable>;
        public:
            using callable_result_t = R;
            using param_type = std::tuple<std::decay_t<Args>...>;
            using result_type = result_t<R, std::exception_ptr>;
    
            // not copyable or copy assignable
            task_impl(const task_impl&) = delete;
    
            task_impl& operator=(const task_impl&) = delete;
    
            task_impl(task_impl&&)
                noexcept(conjunction_v<std::is_nothrow_move_constructible<callable_t>,
                    std::is_nothrow_move_constructible<param_type>>) = default;
    
            task_impl& operator=(task_impl&&)
                noexcept(conjunction_v<std::is_nothrow_move_assignable<callable_t>,
                    std::is_nothrow_move_assignable<param_type>>) = default;
    
            ~task_impl() = default;
    
            void swap(task_impl& rhs) noexcept(noexcept(std::swap(_data, rhs._data))) {
                if (this != &rhs) {
                    using std::swap;
                    swap(_data, rhs._data);
                }
            }
    
            template <typename T = callable_t, typename = std::enable_if_t<std::is_pointer<T>::value>>
            explicit task_impl(T func, Args&&... args) 
                noexcept(conjunction_v<std::is_nothrow_move_constructible<T>, std::is_nothrow_constructible<param_type, Args&&...>>)
                : _data { std::move(func), param_type(std::forward<Args>(args)...) } {
                assert(func);
            }
    
            template <typename T = callable_t, std::enable_if_t<negation_v<std::is_pointer<T>>>* = nullptr>
            explicit task_impl(T func, Args&&... args) 
                noexcept(conjunction_v<std::is_nothrow_move_constructible<T>, std::is_nothrow_constructible<param_type, Args&&...>>)
                : _data { std::move(func), param_type(std::forward<Args>(args)...) } {
            }
    
            template <typename _param_type = param_type>
            std::enable_if_t<sizeof...(Args) != 0, _param_type&> get_params() noexcept { 
                return std::get<data_index::data_params>(_data); 
            }
            
            template <typename _param_type = param_type>
            std::enable_if_t<(sizeof...(Args) != 0), const _param_type&> get_params() const noexcept { 
                return std::get<data_index::data_params>(_data); 
            }
            
            result_type operator()() noexcept { 
                return do_execute(std::index_sequence_for<Args...>(), std::is_same<R, void> {}); 
            }
    
        private:
            template <size_t... idx>
            result_type do_execute(const std::integer_sequence<size_t, idx...>&, std::true_type) noexcept {
                try {
                    auto& callable = std::get<data_index::data_callable>(_data);
                    auto& params = std::get<data_index::data_params>(_data);
                    callable(unpack_param(std::get<idx>(params))...);
                    return result_type(value_tag);
                } catch (...) {
                    return result_type(error_tag, std::current_exception());
                }
            }
    
            template <size_t... idx>
            result_type do_execute(const std::integer_sequence<size_t, idx...>&, std::false_type) noexcept {
                try {
                    auto& callable = std::get<data_index::data_callable>(_data);
                    auto& params = std::get<data_index::data_params>(_data);
                    return result_type(value_tag, callable(unpack_param(std::get<idx>(params))...));
                } catch (...) {
                    return result_type(error_tag, std::current_exception());
                }
            }
            std::tuple<callable_t, param_type> _data;
        };
    
        template <class Obj>
        struct pmf_invoke_obj {
            template <typename T_, bool = is_shared_ptr_v<T_>>
            struct type_getter : type_identity<T_> {};
    
            template <typename T_>
            struct type_getter<T_, true> : type_identity<typename T_::element_type> {};
    
            using raw = std::remove_cv_t<std::remove_reference_t<Obj> >;
            using type = typename std::conditional_t<
                is_shared_ptr<raw>::value,
                type_getter<raw>,
                std::conditional_t<
                    std::is_pointer<raw>::value,
                    type_getter<std::remove_pointer_t<raw> &>, // U* → U&
                    type_getter<raw&> // U → U&
                >
            >::type;
        };
    
        template <class Obj>
        using pmf_invoke_obj_t = typename pmf_invoke_obj<Obj>::type;
    
        template <bool IsMemberFunction, typename Callable, typename ... Args>
        struct task_selector;
    
        template <typename Callable, typename ... Args>
        struct task_selector <false, Callable, Args...> :
                type_identity<task_impl<false, Callable, invoke_result_t<Callable, Args...>, Args...>> {};
    
        template <typename Callable, typename ObjectType, typename ... Args>
        struct task_selector <true, Callable, ObjectType, Args...> :
                type_identity<task_impl<true, Callable, ObjectType,
                    invoke_result_t<Callable, pmf_invoke_obj_t<ObjectType>, Args...>, Args...>> {};
    }

    template <typename Callable, typename ... Args>
    using task = typename task_impl_private::task_selector<
        std::is_member_function_pointer<std::decay_t<Callable>>::value, 
        std::decay_t<Callable>, 
        std::decay_t<Args>...>::type;

    template <typename Callable, typename ... Args>
    void swap(task<Callable, Args...>& lhs, task<Callable, Args...>& rhs)
        noexcept(noexcept(lhs.swap(rhs))) {
        lhs.swap(rhs);
    }

    template<class F, class... A>
    auto make_task(F&& f, A&&... a)
        noexcept(std::is_nothrow_constructible<task<std::decay_t<F>, std::decay_t<A>...>, F&&, A&&...>::value)
        -> task<std::decay_t<F>, std::decay_t<A>...> {
        return task<std::decay_t<F>, std::decay_t<A>...>(std::forward<F>(f), std::forward<A>(a)... );
    }

    template <class F, class... A>
    auto make_unique_task(F&& f, A&&... a)
        -> std::unique_ptr<task<std::decay_t<F>, std::decay_t<A>...>> {
        return std::make_unique<task<std::decay_t<F>, std::decay_t<A>...>>(
            std::forward<F>(f), std::forward<A>(a)...);
    }

    template <class F, class... A>
    auto make_shared_task(F&& f, A&&... a)
        -> std::shared_ptr<task<std::decay_t<F>, std::decay_t<A>...>> {
        return std::make_shared<task<std::decay_t<F>, std::decay_t<A>...>>(
            std::forward<F>(f), std::forward<A>(a)...);
    }

    }

#endif
