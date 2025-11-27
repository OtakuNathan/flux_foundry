//
// Created by nathan on 2025/8/13.
//

#ifndef __LITE_FNDS_CALLABLE_HANDLE_H__
#define __LITE_FNDS_CALLABLE_HANDLE_H__

#include <functional>
#include <utility>

#include "../base/traits.h"
#include "../base/inplace_base.h"
#include "../base/type_erase_base.h"
#include "../memory/result_t.h"

namespace lite_fnds {

namespace callable_handle_impl {
    template <typename R, typename ... Args>
    struct callable_vtable : basic_vtable {
        R (*call)(const void*, Args&&...);

        constexpr callable_vtable(
            fn_copy_construct_t* fcopy,
            fn_move_construct_t* fmove,
            fn_safe_relocate_t* fsafe_reloc,
            fn_destroy_t* fdestroy,
            R (*fcall)(const void*, Args&&...)) noexcept
            : basic_vtable { fcopy, fmove, fsafe_reloc, fdestroy }, call(fcall) {
        }
    };

    template <typename callable, typename R, typename ... Args>
    struct is_compatible {
    private:
        template <typename callable_>
        static auto test(int) -> std::is_convertible<invoke_result_t<const callable_&, Args...>, R>;

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
    public raw_type_erase_base<callable_wrapper<R(Args...)>> {
    using base = raw_type_erase_base<callable_wrapper<R(Args...)>>;
private:
    using callable_vtable = callable_handle_impl::callable_vtable<R, Args...>;

    template <typename T, bool sbo_enabled>
    struct callable_vfns {
        static R call(const void* p, Args&&... args) {
            return (*tr_ptr<const T, sbo_enabled>(p))(std::forward<Args>(args)...);
        }

        static constexpr const callable_vtable* table_for() noexcept {
            static const callable_vtable vt(
                fcopy_construct<T, sbo_enabled>(),
                fmove_construct<T, sbo_enabled>(),
                fsafe_relocate<T, sbo_enabled>(),
                fdestroy<T, sbo_enabled>(),
                &callable_vfns::call);
            return &vt;
        }
    };
    
    result_t<R, std::exception_ptr> do_nothrow(std::true_type, Args&&... args) const noexcept {
        try {
            this->operator()(std::forward<Args>(args)...);
            return result_t<R, std::exception_ptr>(value_tag);
        } catch (...) {
            return result_t<R, std::exception_ptr>(error_tag, std::current_exception());
        }
    }

    result_t<R, std::exception_ptr> do_nothrow(std::false_type, Args&&... args) const noexcept {
        try {
            return result_t<R, std::exception_ptr>(value_tag, this->operator()(std::forward<Args>(args)...));
        } catch (...) {
            return result_t<R, std::exception_ptr>(error_tag, std::current_exception());
        }
    }

public:
    template <typename T, bool sbo_enable>
    void fill_vtable() noexcept {
        this->_vtable = callable_vfns<T, sbo_enable>::table_for();
    }

    callable_wrapper() noexcept = default;
    callable_wrapper(const callable_wrapper&) = default;
    callable_wrapper(callable_wrapper&&) = default;
    callable_wrapper& operator=(const callable_wrapper& rhs) = default;
    callable_wrapper& operator=(callable_wrapper&& rhs) = default;
    ~callable_wrapper() noexcept = default;

    template <typename callable,
        typename callable_storage_t = std::decay_t<callable>,
        typename = std::enable_if_t<conjunction_v<
            callable_handle_impl::is_compatible<callable_storage_t, R, Args...>,
            negation<std::is_same<callable_storage_t, callable_wrapper>>>>>
    callable_wrapper(callable&& f) 
        noexcept(noexcept(this->template emplace<callable_storage_t>(std::declval<callable&&>()))) {
        this->template emplace<callable_storage_t>(std::forward<callable>(f));
    }

    using base::clear;
    using base::emplace;
    using base::swap;

    template <typename callable,
        typename callable_storage_t = std::decay_t<callable>,
        typename = std::enable_if_t<conjunction_v<
            callable_handle_impl::is_compatible<callable_storage_t, R, Args...>,
            negation<std::is_same<callable_storage_t, callable_wrapper>>>>>
    callable_wrapper& operator=(callable&& f) 
        noexcept(noexcept(this->template emplace<callable_storage_t>(std::declval<callable&&>()))) {
        callable_wrapper tmp(std::forward<callable>(f));
        swap(*this, tmp);
        return *this;
    }

    explicit operator bool() const noexcept {
        return this->_vtable != nullptr;
    }

    R operator()(Args&&... args) const {
        if (!this->_vtable) {
            throw std::bad_function_call();
        }
        return static_cast<const callable_vtable*>(this->_vtable)->call(this->_data, std::forward<Args>(args)...);
    }

    result_t<R, std::exception_ptr> nothrow_call(Args&&... args) const noexcept {
        return do_nothrow(std::is_void<R>{}, std::forward<Args>(args)...);
    }
};

template <typename callable>
void swap(callable_wrapper<callable>& a, callable_wrapper<callable>& b) noexcept {
    a.swap(b);
}

}

#endif
