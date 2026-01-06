#ifndef LITE_FNDS_CALLABLE_WRAPPER_H
#define LITE_FNDS_CALLABLE_WRAPPER_H

#include <cassert>
#include <functional>
#include <utility>

#include "../base/traits.h"
#include "../base/inplace_base.h"
#include "../base/type_erase_base.h"
#include "../memory/result_t.h"

namespace lite_fnds {

    namespace callable_handle_impl {
        template <typename callable, typename R, typename ... Args>
        struct is_callable_and_compatible {
        private:
            template <typename callable_>
            static auto test(int) ->
#if LFNDS_HAS_EXCEPTIONS
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

        template <typename wrapper>
        struct callable_storage_t :
            raw_type_erase_base<callable_storage_t<wrapper>, CACHE_LINE_SIZE - 4 * sizeof(std::nullptr_t)> {
            using base = raw_type_erase_base<callable_storage_t<wrapper>, CACHE_LINE_SIZE - 4 * sizeof(std::nullptr_t)>;
            using callable_vtable = basic_vtable;

            template <typename T, bool sbo_enabled>
            using callable_vfns = typename wrapper::template callable_vfns<T, sbo_enabled>;
            using invoker_t = typename wrapper::invoker_t;
            invoker_t invoker_ = nullptr;

            template <typename callable,
                typename = std::enable_if_t<!is_self_constructing<callable_storage_t, callable>::value>>
            callable_storage_t(callable&& f)
                noexcept(noexcept(std::declval<base&>().
                    template emplace<callable>(std::declval<callable&&>()))) {
                this->template emplace<callable>(std::forward<callable>(f));
            }

            template <typename T, bool sbo_enable>
            void fill_vtable() noexcept {
                this->_vtable = callable_vfns<T, sbo_enable>::table_for();
                this->invoker_ = callable_vfns<T, sbo_enable>::call;
            }

            using base::base;
            using base::emplace;
#if LFNDS_COMPILER_HAS_EXCEPTIONS
            callable_storage_t(const callable_storage_t& rhs) = default;
            callable_storage_t& operator=(const callable_storage_t& rhs) = default;
#else
            callable_storage_t(const callable_storage_t& rhs)
                noexcept : base() {
                if (rhs._vtable) {
                    if (!rhs._vtable->copy_construct) {
                        std::abort();
                    }

                    rhs._vtable->copy_construct(this->_data, rhs._data);
                    this->_vtable = rhs._vtable;
                    this->invoker_ = rhs.invoker_;
                }
            }

            callable_storage_t& operator=(const callable_storage_t& rhs) noexcept {
                if (this == &rhs) {
                    return *this;
                }
                callable_storage_t tmp(rhs);
                this->swap(tmp);
                return *this;
            }

#endif
            callable_storage_t(callable_storage_t&& rhs) noexcept : base() {
                if (!rhs._vtable) {
                    this->_vtable = nullptr;
                } else {
                    rhs._vtable->safe_relocate(this->_data, rhs._data);
                    this->_vtable = rhs._vtable;
                    invoker_ = rhs.invoker_;
                    rhs._vtable = nullptr;
                    rhs.invoker_ = nullptr;
                }
            }

            callable_storage_t& operator=(callable_storage_t&& rhs) noexcept {
                if (this == &rhs) {
                    return *this;
                }
                // clear current
                this->clear();
                if (rhs._vtable) {
                    rhs._vtable->safe_relocate(this->_data, rhs._data);
                    this->_vtable = rhs._vtable;
                    invoker_ = rhs.invoker_;
                    rhs._vtable = nullptr;
                    rhs.invoker_ = nullptr;
                }
                return *this;
            }

            ~callable_storage_t() noexcept {
                clear();
                invoker_ = nullptr;
            }

            void clear() noexcept {
                base::clear();
                invoker_ = nullptr;
            }

            void swap(callable_storage_t& rhs) noexcept {
                base::swap(rhs);
                using std::swap;
                swap(invoker_, rhs.invoker_);
            }
        };

        template <typename wrapper>
        void swap(callable_storage_t<wrapper>& a, callable_storage_t<wrapper>& b) noexcept {
            a.swap(b);
        }
    }
    using callable_handle_impl::swap;

    template <typename>
    class callable_wrapper;

    // this is not thread safe
    template <typename R, typename ... Args>
    class callable_wrapper <R(Args...)> {
        template <typename T, bool sbo_enabled>
        struct callable_vfns {
            static R call(void* p, Args... args) {
                return (*tr_ptr<T, sbo_enabled>(p))(std::forward<Args>(args)...);
            }

            static const basic_vtable* table_for() noexcept {
                static const basic_vtable vt{
                        fcopy_construct<T, sbo_enabled>(),
                        fmove_construct<T, sbo_enabled>(),
                        fsafe_relocate<T, sbo_enabled>(),
                        fdestroy<T, sbo_enabled>()
                };
                return &vt;
            }
        };

        using invoker_t = R(*)(void*, Args...);
        using fp_t = R(*)(Args...);
        using callable_storage_t = callable_handle_impl::callable_storage_t<callable_wrapper>;
        friend struct callable_handle_impl::callable_storage_t<callable_wrapper>;

        using storage_t = either_t<fp_t, callable_storage_t>;
        storage_t storage_ = storage_t(to_first, nullptr);

#if LFNDS_COMPILER_HAS_EXCEPTIONS
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
                return result_t<R, std::exception_ptr>(value_tag, this->operator()(args...));
            } catch (...) {
                return result_t<R, std::exception_ptr>(error_tag, std::current_exception());
            }
        }
#endif
    public:
        callable_wrapper() noexcept = default;
        callable_wrapper(const callable_wrapper&) = default;
        callable_wrapper(callable_wrapper&&) noexcept = default;
        callable_wrapper& operator=(const callable_wrapper& rhs) = default;
        callable_wrapper& operator=(callable_wrapper&& rhs) noexcept = default;
        ~callable_wrapper() noexcept = default;

        template <typename callable,
            typename callable_t = std::decay_t<callable>,
            typename = std::enable_if_t<conjunction_v<
            negation<is_self_constructing<callable_wrapper, callable_t>>,
            callable_handle_impl::is_callable_and_compatible<callable_t, R, Args...>>>>
        callable_wrapper(callable&& f)
            noexcept(noexcept(std::declval<callable_wrapper&>().emplace(std::declval<callable&&>()))) {
            emplace(std::forward<callable>(f));
        }

        template <typename callable,
            typename callable_t = std::decay_t<callable>,
            std::enable_if_t<conjunction_v<
            std::is_convertible<callable_t, R(*)(Args...)>,
        callable_handle_impl::is_callable_and_compatible<callable_t, R, Args...>,
            negation<is_self_constructing<callable_wrapper, callable_t>>>>* = nullptr>
        void emplace(callable&& f) noexcept {
            storage_.emplace_first(std::forward<callable>(f));
        }

        template <typename callable,
            typename callable_t = std::decay_t<callable>,
            std::enable_if_t<conjunction_v<
            negation<std::is_convertible<callable_t, R(*)(Args...)>>,
        callable_handle_impl::is_callable_and_compatible<callable_t, R, Args...>,
            negation<is_self_constructing<callable_wrapper, callable_t>>>>* = nullptr>
            void emplace(callable&& f)
            noexcept(noexcept(std::declval<storage_t&>().emplace_second(std::declval<callable&&>()))) {
            storage_.emplace_second(std::forward<callable>(f));
        }

        void clear() noexcept {
            storage_.emplace_first(nullptr);
        }

        void swap(callable_wrapper& rhs)
            noexcept(is_nothrow_swappable<storage_t>::value) {
            using std::swap;
            swap(storage_, rhs.storage_);
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
            swap(tmp);
            return *this;
        }

        explicit operator bool() const noexcept {
            return !storage_.has_first() || storage_.get_first();
        }

        FORCE_INLINE R operator()(Args... args)
#if !LFNDS_COMPILER_HAS_EXCEPTIONS
            noexcept
#endif
        {
#if LFNDS_COMPILER_HAS_EXCEPTIONS
            UNLIKELY_IF(!*this) {
                throw std::bad_function_call();
            }
#else
            assert(*this && "attempting to call an uninitialized callable wrapper.");
#endif

            LIKELY_IF(storage_.has_first()) {
                return storage_.get_first()(args...);
            }

            auto& st = storage_.get_second();
            return st.invoker_(st._data, args...);

        }
#if LFNDS_COMPILER_HAS_EXCEPTIONS
        FORCE_INLINE result_t<R, std::exception_ptr> nothrow_call(Args... args) noexcept {
            return do_nothrow(std::is_void<R>{}, args...);
        }
#endif
    };

    template <typename R, typename ... Args>
    class callable_wrapper <R(Args...) const> {
        template <typename T, bool sbo_enabled>
        struct callable_vfns {
            static R call(const void* p, Args... args) {
                return (*tr_ptr<T, sbo_enabled>(p))(std::forward<Args>(args)...);
            }

            static const basic_vtable* table_for() noexcept {
                static const basic_vtable vt{
                        fcopy_construct<T, sbo_enabled>(),
                        fmove_construct<T, sbo_enabled>(),
                        fsafe_relocate<T, sbo_enabled>(),
                        fdestroy<T, sbo_enabled>()
                };
                return &vt;
            }
        };

        using invoker_t = R(*)(const void*, Args...);
        using fp_t = R(*)(Args...);
        using callable_storage_t = callable_handle_impl::callable_storage_t<callable_wrapper>;
        friend struct callable_handle_impl::callable_storage_t<callable_wrapper>;
        using storage_t = either_t<fp_t, callable_storage_t>;
        storage_t storage_ = storage_t(to_first, nullptr);

#if LFNDS_COMPILER_HAS_EXCEPTIONS
        result_t<R, std::exception_ptr> do_nothrow(std::true_type, Args... args) const noexcept {
            try {
                this->operator()(std::forward<Args>(args)...);
                return result_t<R, std::exception_ptr>(value_tag);
            } catch (...) {
                return result_t<R, std::exception_ptr>(error_tag, std::current_exception());
            }
        }

        result_t<R, std::exception_ptr> do_nothrow(std::false_type, Args ... args) const noexcept {
            try {
                return result_t<R, std::exception_ptr>(value_tag, this->operator()(args...));
            } catch (...) {
                return result_t<R, std::exception_ptr>(error_tag, std::current_exception());
            }
        }
#endif
    public:
        callable_wrapper() noexcept = default;
        callable_wrapper(const callable_wrapper&) = default;
        callable_wrapper(callable_wrapper&&) noexcept = default;
        callable_wrapper& operator=(const callable_wrapper& rhs) = default;
        callable_wrapper& operator=(callable_wrapper&& rhs) noexcept = default;
        ~callable_wrapper() noexcept = default;

        template <typename callable,
            typename callable_t = std::decay_t<callable>,
            typename = std::enable_if_t<conjunction_v<
            negation<is_self_constructing<callable_wrapper, callable_t>>,
            callable_handle_impl::is_callable_and_compatible<const callable_t, R, Args...>>>>
        callable_wrapper(callable&& f)
            noexcept(noexcept(std::declval<callable_wrapper&>().emplace(std::declval<callable&&>()))) {
            emplace(std::forward<callable>(f));
        }

        template <typename callable,
            typename callable_t = std::decay_t<callable>,
            std::enable_if_t<conjunction_v<
            std::is_convertible<callable_t, R(*)(Args...)>,
        callable_handle_impl::is_callable_and_compatible<const callable_t, R, Args...>,
            negation<is_self_constructing<callable_wrapper, callable_t>>>>* = nullptr>
        void emplace(callable&& f) noexcept {
            storage_.emplace_first(std::forward<callable>(f));
        }

        template <typename callable,
            typename callable_t = std::decay_t<callable>,
            std::enable_if_t<conjunction_v<
            negation<std::is_convertible<callable_t, R(*)(Args...)>>,
            callable_handle_impl::is_callable_and_compatible<const callable_t, R, Args...>,
            negation<is_self_constructing<callable_wrapper, callable_t>>>>* = nullptr>
        void emplace(callable&& f)
            noexcept(noexcept(std::declval<storage_t&>().emplace_second(std::declval<callable&&>()))) {
            storage_.emplace_second(std::forward<callable>(f));
        }

        void clear() noexcept {
            storage_.emplace_first(nullptr);
        }

        void swap(callable_wrapper& rhs)
            noexcept(is_nothrow_swappable<storage_t>::value) {
            using std::swap;
            swap(storage_, rhs.storage_);
        }

        template <typename callable,
            typename callable_t = std::decay_t<callable>,
            typename = std::enable_if_t<conjunction_v<
                negation<is_self_constructing<callable_wrapper, callable_t>>,
                callable_handle_impl::is_callable_and_compatible<const callable_t, R, Args...>
            >>>
            callable_wrapper& operator=(callable&& f)
            noexcept(noexcept(std::declval<callable_wrapper&>().
                template emplace<callable>(std::declval<callable&&>()))) {
            callable_wrapper tmp(std::forward<callable>(f));
            swap(tmp);
            return *this;
        }

        explicit operator bool() const noexcept {
            return !storage_.has_first() || storage_.get_first();
        }

        FORCE_INLINE R operator()(Args... args) const
#if !LFNDS_COMPILER_HAS_EXCEPTIONS
            noexcept
#endif
        {
#if LFNDS_COMPILER_HAS_EXCEPTIONS
            UNLIKELY_IF(!*this) {
                throw std::bad_function_call();
            }
#else
            assert(*this && "attempting to call an uninitialized callable wrapper.");
#endif

            LIKELY_IF(storage_.has_first()) {
                return storage_.get_first()(args...);
            }

            auto& st = storage_.get_second();
            return st.invoker_(st._data, args...);
        }

#if LFNDS_COMPILER_HAS_EXCEPTIONS
        FORCE_INLINE result_t<R, std::exception_ptr> nothrow_call(Args... args) const noexcept {
            return do_nothrow(std::is_void<R>{}, args...);
        }
#endif
    };

    template <typename callable>
    void swap(callable_wrapper<callable>& a, callable_wrapper<callable>& b) noexcept {
        a.swap(b);
    }

}

#endif