#ifndef FLUX_FOUNDRY_FLOW_DEFINE_H
#define FLUX_FOUNDRY_FLOW_DEFINE_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

namespace flux_foundry {
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

// Classification of errors synthesized by the library.
enum class flow_error_code : uint8_t {
    cancel_soft,
    cancel_hard,
    awaitable_create_failed,
    async_submission_failed,
    all_async_failed,
    any_async_failed,
};

inline const char* flow_error_code_message(flow_error_code code) noexcept {
    switch (code) {
        case flow_error_code::cancel_soft:             return "flow soft-canceled";
        case flow_error_code::cancel_hard:             return "flow hard-canceled";
        case flow_error_code::awaitable_create_failed: return "failed to create awaitable";
        case flow_error_code::async_submission_failed: return "failed to submit async operation";
        case flow_error_code::all_async_failed:        return "all async operations are failed.";
        case flow_error_code::any_async_failed:        return "async operation failed";
    }
    return "flow error";
}

#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
class flow_error : public std::logic_error {
public:
    flow_error(flow_error_code code, size_t index = 0, int submit_code = 0)
        : std::logic_error(make_message(code, index)),
          code_(code),
          submit_code_(submit_code),
          index_(index) {}

    flow_error_code code() const noexcept { return code_; }
    size_t index() const noexcept { return index_; }
    int submit_code() const noexcept { return submit_code_; }

private:
    static std::logic_error make_message(flow_error_code code, size_t index) {
        if (code == flow_error_code::any_async_failed) {
            char message[256];
            std::snprintf(message, sizeof(message), "async operation #%zu failed", index);
            return std::logic_error(message);
        }
        return std::logic_error(flow_error_code_message(code));
    }

    flow_error_code code_;
    int submit_code_;
    size_t index_;
};
#endif

template <typename E>
struct cancel_error {
    static E make(cancel_kind) {
        static_assert(sizeof(E) == 0,
            "flux_foundry::cancel_error<E> is not specialized for this error type E. "
            "Please provide `template<> struct flux_foundry::cancel_error<E>` "
            "with a static `E make(cancel_kind)` member.");
    }
};

#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
template <>
struct cancel_error<std::exception_ptr> {
    static std::exception_ptr make(cancel_kind kind) noexcept {
        const flow_error_code code = (kind == cancel_kind::hard)
            ? flow_error_code::cancel_hard
            : flow_error_code::cancel_soft;
        try {
            return std::make_exception_ptr(flow_error(code));
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
            "flux_foundry::awaitable_creating_error<E> is not specialized for this error type E. "
            "Please provide `template<> struct flux_foundry::awaitable_creating_error<E>` "
            "with a static `E make()` member.");
    }
};

#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
template <>
struct awaitable_creating_error<std::exception_ptr> {
    static std::exception_ptr make() noexcept {
        try {
            return std::make_exception_ptr(flow_error(flow_error_code::awaitable_create_failed));
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
            "flux_foundry::async_submission_failed_error<E> is not specialized for this error type E. "
            "Please provide `template<> struct flux_foundry::async_submission_failed_error<E>` "
            "with a static `E make(int)` or `E make()` member.");
    }
};

#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
template <>
struct async_submission_failed_error<std::exception_ptr> {
    static std::exception_ptr make(int submit_code) noexcept {
        try {
            return std::make_exception_ptr(flow_error(flow_error_code::async_submission_failed, 0, submit_code));
        } catch (...) {
            return std::current_exception();
        }
    }

    static std::exception_ptr make() noexcept {
        return make(0);
    }
};
#endif

template <typename E>
struct async_all_failed_error {
    static E make() {
        static_assert(sizeof(E) == 0,
            "flux_foundry::async_all_failed_error<E> is not specialized for this error type E. "
            "Please provide `template<> struct flux_foundry::async_all_failed_error<E>` "
            "with a static `E make()` member.");
    }
};

#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
template <>
struct async_all_failed_error<std::exception_ptr> {
    static std::exception_ptr make() {
        try {
            return std::make_exception_ptr(flow_error(flow_error_code::all_async_failed));
        } catch (...) {
            return std::current_exception();
        }
    }
};
#endif

template <typename E>
struct async_any_failed_error {
        static E make(size_t) {
            static_assert(sizeof(E) == 0,
                "flux_foundry::async_any_failed_error<E> is not specialized for this error type E. "
                "Please provide `template<> struct flux_foundry::async_any_failed_error<E>` "
                "with a static `E make(size_t i)` member.");
        }
    };

#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    template <>
    struct async_any_failed_error<std::exception_ptr> {
        static std::exception_ptr make(size_t i) {
            try {
                return std::make_exception_ptr(
                    flow_error(flow_error_code::any_async_failed, i));
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
