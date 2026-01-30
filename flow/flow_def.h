#ifndef LITE_FNDS_FLOW_DEFINE_H
#define LITE_FNDS_FLOW_DEFINE_H

#include <stdexcept>

namespace lite_fnds {
namespace flow_impl {
#if defined(__clang__)
    static constexpr size_t MAX_ZIP_N = 2;
#elif defined(__GNUC__)
    static constexpr size_t MAX_ZIP_N = 2;
#elif defined(_MSC_VER)
    static constexpr size_t MAX_ZIP_N = 8;
#else
    static constexpr size_t MAX_ZIP_N = 2;
#endif
}

enum class cancel_kind {
    soft,
    hard,
};

template <typename E>
struct cancel_error {
    static E make(cancel_kind) {
        static_assert(sizeof(E) == 0,
            "lite_fnds::cancel_error<E> is not specialized for this error type E. "
            "Please provide `template<> struct lite_fnds::cancel_error<E>` "
            "with a static `E make(cancel_kind)` member.");
    }
};

template <>
struct cancel_error<std::exception_ptr> {
    static std::exception_ptr make(cancel_kind kind) {
        const char* msg = (kind == cancel_kind::hard)
            ? "flow hard-canceled"
            : "flow soft-canceled";
        return std::make_exception_ptr(std::logic_error(msg));
    }
};


template <typename E>
struct awaitable_creating_error {
    static E make() noexcept {
        static_assert(sizeof(E) == 0,
            "lite_fnds::awaitable_creating_error<E> is not specialized for this error type E. "
            "Please provide `template<> struct lite_fnds::awaitable_creating_error<E>` "
            "with a static `E make()` member.");
    }
};

template <>
struct awaitable_creating_error<std::exception_ptr> {
    static std::exception_ptr make() noexcept {
        return std::make_exception_ptr(std::logic_error("failed to create awaitable"));
    }
};

template <typename E>
struct async_submission_failed_error {
    static E make() {
        static_assert(sizeof(E) == 0,
            "lite_fnds::async_submission_failed_error<E> is not specialized for this error type E. "
            "Please provide `template<> struct lite_fnds::async_submission_failed_error<E>` "
            "with a static `E make()` member.");
    }
};

template <>
struct async_submission_failed_error<std::exception_ptr> {
    static std::exception_ptr make() {
        return std::make_exception_ptr(std::logic_error("failed to submit async operation"));
    }
};

using flow_async_cancel_handler_t = void (*)(void*, cancel_kind);
using flow_async_notify_handler_dropped_t = void(*)(void*);
using flow_async_cancel_param_t = void*;

}
#endif
