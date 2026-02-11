#ifndef FLUX_FOUNDRY_CALLABLE_WRAPPER_H
#define FLUX_FOUNDRY_CALLABLE_WRAPPER_H

#include <cassert>
#include <functional>
#include <utility>

#include "../base/traits.h"
#include "../base/inplace_base.h"
#include "../base/type_erase_base.h"
#include "../memory/result_t.h"

namespace flux_foundry {
    constexpr static size_t callable_wrapper_sbo_size = 48;

    namespace callable_handle_impl {
        template <typename callable, typename R, typename ... Args>
        struct is_callable_and_compatible {
        private:
            template <typename callable_>
            static auto test(int) ->
#if FLUEX_FOUNDRY_HAS_EXCEPTIONS
                std::is_convertible<invoke_result_t<callable_&, Args...>, R>;
#else
                conjunction< std::integral_constant<bool,
                noexcept(std::declval<callable_&>()(std::declval<Args>()...))>,
                std::is_convertible<invoke_result_t<callable_&, Args...>, R>>;
#endif

            template <typename ...>
            static auto test(...) -> std::false_type;
        public:
            constexpr static bool value = decltype(test<callable>(0))::value;
        };
    }

    template <typename>
    class callable_wrapper;

    // this is not thread safe
    template <typename R, typename ... Args>
    class callable_wrapper <R(Args...)> : 
        public raw_type_erase_base<callable_wrapper<R(Args...)>, callable_wrapper_sbo_size,
            alignof(std::max_align_t), R(void*, Args...)> {

        template <typename T, bool sbo_enabled>
        struct callable_vfns {
            static R call(void* p, Args... args) {
                return (*tr_ptr<T, sbo_enabled>(p))(std::forward<Args>(args)...);
            }
        };

        using base = raw_type_erase_base<callable_wrapper<R(Args...)>, callable_wrapper_sbo_size,
            alignof(std::max_align_t), R(void*, Args...)>;


        static R stub(void*, Args... args) {
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            throw std::bad_function_call();
#else
            assert(false && "attempting to call an uninitialized callable wrapper.");
            std::abort();
#endif
        }

        friend struct raw_type_erase_base<callable_wrapper<R(Args...)>, callable_wrapper_sbo_size,
            alignof(std::max_align_t), R(void*, Args...)>;
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        result_t<R, std::exception_ptr> do_nothrow(std::true_type, Args... args) noexcept {
            try {
                this->operator()(std::forward<Args>(args)...);
                return result_t<R, std::exception_ptr>(value_tag);
            } catch (...) {
                return result_t<R, std::exception_ptr>(error_tag, std::current_exception());
            }
        }

        result_t<R, std::exception_ptr> do_nothrow(std::false_type, Args ... args) noexcept {
            try {
                return result_t<R, std::exception_ptr>(value_tag, this->operator()(std::forward<Args>(args)...));
            } catch (...) {
                return result_t<R, std::exception_ptr>(error_tag, std::current_exception());
            }
        }
#endif
    public:
        callable_wrapper() noexcept = default;

        template <typename T, bool sbo_enabled>
        void do_customize() noexcept {
            this->invoker_ = callable_vfns<T, sbo_enabled>::call;
            this->manager_ = life_span_manager<T, sbo_enabled>::manage;
        }

        template <typename callable,
            typename callable_t = std::decay_t<callable>,
            typename = std::enable_if_t<conjunction_v<
                negation<is_self_constructing<callable_wrapper, callable_t>>,
                callable_handle_impl::is_callable_and_compatible<callable_t, R, Args...>>>>
        callable_wrapper(callable&& f)
            noexcept(noexcept(std::declval<base&>(). template emplace<callable>(std::declval<callable&&>()))) {
            this->template emplace<callable>(std::forward<callable>(f));
        }

        template <typename callable,
            typename callable_t = std::decay_t<callable>,
            typename = std::enable_if_t<conjunction_v<
            negation<is_self_constructing<callable_wrapper, callable>>,
            callable_handle_impl::is_callable_and_compatible<callable_t, R, Args...>>
        >>
        callable_wrapper& operator=(callable&& f)
            noexcept(noexcept(std::declval<callable_wrapper&>().
                template emplace<callable>(std::declval<callable&&>()))) {
            callable_wrapper tmp(std::forward<callable>(f));
            this->swap(tmp);
            return *this;
        }

        FORCE_INLINE R operator()(Args... args)
#if !FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            noexcept
#endif
        {
            return this->invoker_(this->data_, std::forward<Args>(args)...);
        }

#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        FORCE_INLINE result_t<R, std::exception_ptr> nothrow_call(Args... args) noexcept {
            return do_nothrow(std::is_void<R>{}, std::forward<Args>(args)...);
        }
#endif
    };

    template <typename R, typename ... Args>
    class callable_wrapper <R(Args...) const> 
        : public raw_type_erase_base<callable_wrapper<R(Args...) const>, callable_wrapper_sbo_size,
            alignof(std::max_align_t), R(const void*, Args...)> {
        template <typename T, bool sbo_enabled>
        struct callable_vfns {
            static R call(const void* p, Args... args) {
                return (*tr_ptr<T, sbo_enabled>(p))(std::forward<Args>(args)...);
            }
        };

        using base = raw_type_erase_base<callable_wrapper<R(Args...) const>, callable_wrapper_sbo_size,
            alignof(std::max_align_t), R(const void*, Args...)>;

        static R stub(const void*, Args... args) {
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            throw std::bad_function_call();
#else
            assert(false && "attempting to call an uninitialized callable wrapper.");
            std::abort();
#endif
        }

        friend struct raw_type_erase_base<callable_wrapper<R(Args...) const>, callable_wrapper_sbo_size,
            alignof(std::max_align_t), R(const void*, Args...)>;

#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        result_t<R, std::exception_ptr> do_nothrow(std::true_type, Args... args) const noexcept {
            try {
                this->operator()(std::forward<Args>(args)...);
                return result_t<R, std::exception_ptr>(value_tag);
            } catch (...) {
                return result_t<R, std::exception_ptr>(error_tag, std::current_exception());
            }
        }

        result_t<R, std::exception_ptr> do_nothrow(std::false_type, Args... args) const noexcept {
            try {
                return result_t<R, std::exception_ptr>(value_tag, this->operator()(std::forward<Args>(args)...));
            } catch (...) {
                return result_t<R, std::exception_ptr>(error_tag, std::current_exception());
            }
        }
#endif
    public:
        callable_wrapper() noexcept = default;

        template <typename T, bool sbo_enabled>
        void do_customize() noexcept {
            this->invoker_ = callable_vfns<T, sbo_enabled>::call;
            this->manager_ = life_span_manager<T, sbo_enabled>::manage;
        }

        template <typename callable,
            typename callable_t = std::decay_t<callable>,
            typename = std::enable_if_t<conjunction_v<
                negation<is_self_constructing<callable_wrapper, callable_t>>,
                callable_handle_impl::is_callable_and_compatible<const callable_t, R, Args...>>>>
        callable_wrapper(callable&& f) 
            noexcept(noexcept(std::declval<base&>().template emplace<callable>(std::declval<callable&&>()))) {
            this->template emplace<callable>(std::forward<callable>(f));
        }

        template <typename callable,
            typename callable_t = std::decay_t<callable>,
            typename = std::enable_if_t<conjunction_v<
                negation<is_self_constructing<callable_wrapper, callable>>,
                callable_handle_impl::is_callable_and_compatible<const callable_t, R, Args...>>>>
        callable_wrapper& operator=(callable&& f) 
            noexcept(noexcept(std::declval<base&>().template emplace<callable>(std::declval<callable&&>()))) {
            callable_wrapper tmp(std::forward<callable>(f));
            this->swap(tmp);
            return *this;
        }

        FORCE_INLINE R operator()(Args... args) const
#if !FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            noexcept
#endif
        {
            return this->invoker_(this->data_, std::forward<Args>(args)...);
        }

#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        FORCE_INLINE result_t<R, std::exception_ptr> nothrow_call(Args... args) const noexcept {
            return do_nothrow(std::is_void<R> {}, std::forward<Args>(args)...);
        }
#endif
    };

    template <typename callable>
    void swap(callable_wrapper<callable>& a, callable_wrapper<callable>& b) noexcept {
        a.swap(b);
    }
}

#endif
