#ifndef LITE_FNDS_FLOW_DEFINE_H
#define LITE_FNDS_FLOW_DEFINE_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
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

#if LFNDS_COMPILER_HAS_EXCEPTIONS
template <>
struct cancel_error<std::exception_ptr> {
    static std::exception_ptr make(cancel_kind kind) noexcept {
        const char* msg = (kind == cancel_kind::hard)
            ? "flow hard-canceled"
            : "flow soft-canceled";
        try {
            return std::make_exception_ptr(std::logic_error(msg));
        } catch (...) {
            return std::current_exception();
        }
    }
};
#endif

template <typename E>
struct awaitable_creating_error {
    static E make() noexcept {
        static_assert(sizeof(E) == 0,
            "lite_fnds::awaitable_creating_error<E> is not specialized for this error type E. "
            "Please provide `template<> struct lite_fnds::awaitable_creating_error<E>` "
            "with a static `E make()` member.");
    }
};

#if LFNDS_COMPILER_HAS_EXCEPTIONS
template <>
struct awaitable_creating_error<std::exception_ptr> {
    static std::exception_ptr make() noexcept {
        try {
            return std::make_exception_ptr(std::logic_error("failed to create awaitable"));
        } catch (...) {
            return std::current_exception();
        }
    }
};
#endif

template <typename E>
struct async_submission_failed_error {
    static E make() {
        static_assert(sizeof(E) == 0,
            "lite_fnds::async_submission_failed_error<E> is not specialized for this error type E. "
            "Please provide `template<> struct lite_fnds::async_submission_failed_error<E>` "
            "with a static `E make()` member.");
    }
};

#if LFNDS_COMPILER_HAS_EXCEPTIONS
template <>
struct async_submission_failed_error<std::exception_ptr> {
    static std::exception_ptr make() noexcept {
        try {
            return std::make_exception_ptr(std::logic_error("failed to submit async operation"));
        } catch (...) {
            return std::current_exception();
        }
    }
};
#endif

template <typename E>
struct async_all_failed_error {
    static E make() {
        static_assert(sizeof(E) == 0,
            "lite_fnds::async_all_failed_error<E> is not specialized for this error type E. "
            "Please provide `template<> struct lite_fnds::async_all_failed_error<E>` "
            "with a static `E make()` member.");
    }
};

#if LFNDS_COMPILER_HAS_EXCEPTIONS
template <>
struct async_all_failed_error<std::exception_ptr> {
    static std::exception_ptr make() {
        try {
            return std::make_exception_ptr(std::logic_error("all async operations are failed."));
        } catch (...) {
            return std::current_exception();
        }
    }
};
#endif

template <typename E>
struct async_any_failed_error {
        static E make(size_t i) {
            static_assert(sizeof(E) == 0,
                "lite_fnds::async_any_failed_error<E> is not specialized for this error type E. "
                "Please provide `template<> struct lite_fnds::async_any_failed_error<E>` "
                "with a static `E make(size_t i)` member.");
        }
    };

#if LFNDS_COMPILER_HAS_EXCEPTIONS
    template <>
    struct async_any_failed_error<std::exception_ptr> {
        static std::exception_ptr make(size_t i) {
            try {
                char buff[256]{};
                snprintf(buff, sizeof(buff), "async operation #%zu failed", i);
                return std::make_exception_ptr(std::logic_error(buff));
            } catch (...) {
                return std::current_exception();
            }
        }
    };
#endif

namespace detail {
    using flow_async_cancel_handler_t = void (*)(void*, cancel_kind);
    using flow_async_notify_handler_dropped_t = void(*)(void*);
    using flow_async_cancel_param_t = void*;

    enum class slot_state : uint8_t {
        empty,
        occupied,
        full,
    };
}


}
#endif
