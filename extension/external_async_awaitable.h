#ifndef FLUX_FOUNDRY_EXTERNAL_ASYNC_AWAITABLE_H
#define FLUX_FOUNDRY_EXTERNAL_ASYNC_AWAITABLE_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "../flow/flow_awaitable.h"
#include "../flow/flow_node.h"
#include "../memory/lite_ptr.h"

#if !FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
#include <system_error>
#endif

namespace flux_foundry {
namespace extension {

#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
using external_async_error_t = std::exception_ptr;
#else
using external_async_error_t = std::error_code;
#endif

using external_async_callback_fp_t = void (*)(void*);
using external_async_callback_param_t = void*;

namespace detail {

// check OP::context_t
template<typename OP, typename = void>
struct context_type_prob : std::false_type {};

template<typename OP>
struct context_type_prob<OP, void_t<typename OP::context_t>> : conjunction<
    std::is_standard_layout<typename OP::context_t>,
    std::is_trivially_copy_constructible<typename OP::context_t>,
    std::is_trivially_destructible<typename OP::context_t>> {};

template<typename OP>
constexpr bool context_type_prob_v = context_type_prob<OP>::value;

// check int OP::init_ctx(context_t*, value_type*) noexcept;
template<typename OP, typename param_t, typename = void>
struct context_init_ctx_prob : std::false_type {};

template<typename OP, typename param_t>
struct context_init_ctx_prob<OP, param_t, void_t<decltype(OP::init_ctx)>> : conjunction<
    std::is_convertible<invoke_result_t<decltype(&OP::init_ctx), typename OP::context_t*, param_t>, int>,
    std::integral_constant<bool, noexcept(OP::init_ctx(
        std::declval<typename OP::context_t*>(),
        std::declval<param_t>()))>> {};

template<typename OP, typename param_t>
constexpr bool context_init_ctx_prob_v = context_init_ctx_prob<OP, param_t>::value;

// check void OP::destroy_ctx(context_t*) noexcept;
template<typename OP, typename = void>
struct context_destroy_ctx_prob : std::false_type {};

template<typename OP>
struct context_destroy_ctx_prob<OP, void_t<decltype(OP::destroy_ctx)>> : conjunction<
    std::is_void<invoke_result_t<decltype(&OP::destroy_ctx), typename OP::context_t*>>,
    std::integral_constant<bool, noexcept(OP::destroy_ctx(
        std::declval<typename OP::context_t*>()))>> {};

template<typename OP>
constexpr bool context_destroy_ctx_prob_v = context_destroy_ctx_prob<OP>::value;

// check void OP::free_result(result_t) noexcept;
template<typename OP, typename = void>
struct context_free_result_prob : std::false_type {};

template<typename OP>
struct context_free_result_prob<OP, void_t<decltype(OP::free_result)>> : conjunction<
    std::is_void<invoke_result_t<decltype(&OP::free_result), typename OP::result_t>>,
    std::integral_constant<bool, noexcept(OP::free_result(std::declval<typename OP::result_t>()))>> {};

template<typename OP>
constexpr bool context_free_result_prob_v = context_free_result_prob<OP>::value;

// check int OP::submit(context_t*, callback, user_data) noexcept;
template<typename OP, typename = void>
struct submit_prob : std::false_type {};

template<typename OP>
struct submit_prob<OP, void_t<decltype(OP::submit)>> : conjunction<
    std::is_convertible<invoke_result_t<decltype(&OP::submit),
        typename OP::context_t*, external_async_callback_fp_t, external_async_callback_param_t>, int>,
    std::integral_constant<bool, noexcept(OP::submit(
        std::declval<typename OP::context_t*>(),
        std::declval<external_async_callback_fp_t>(),
        std::declval<external_async_callback_param_t>()))>> {};

template<typename OP>
constexpr bool submit_prob_v = submit_prob<OP>::value;

// check result_t OP::collect(context_t*) noexcept;
template<typename OP, typename = void>
struct collect_prob : std::false_type {};

template<typename OP>
struct collect_prob<OP, void_t<decltype(OP::collect)>> : conjunction<
    std::is_same<invoke_result_t<decltype(&OP::collect), typename OP::context_t*>, typename OP::result_t>,
    std::integral_constant<bool, noexcept(OP::collect(
        std::declval<typename OP::context_t*>()))>> {};

template<typename OP>
constexpr bool collect_prob_v = collect_prob<OP>::value;

} // namespace detail

template<typename external_async_operator_t>
struct external_async_result_deleter {
    void operator()(typename external_async_operator_t::result_t result) noexcept {
        external_async_operator_t::free_result(result);
    }
};

template<typename external_async_operator_t>
struct external_async_awaitable final :
    fast_awaitable_base<
        external_async_awaitable<external_async_operator_t>,
        std::unique_ptr<
            std::remove_pointer_t<typename external_async_operator_t::result_t>,
            external_async_result_deleter<external_async_operator_t>>,
        external_async_error_t> {
    static_assert(std::is_pointer<typename external_async_operator_t::result_t>::value,
        "the result of external async operation should be a pointer of a struct or something else\n");

    static_assert(detail::context_free_result_prob_v<external_async_operator_t>,
        "there should be static function whose signature is like:"
        "void external_async_operator_t::free_result(typename external_async_operator_t::result_t) noexcept;\n"
        "which is used to free the result of the external async operation.\n");

    using async_result_type = result_t<
        std::unique_ptr<
            std::remove_pointer_t<typename external_async_operator_t::result_t>,
            external_async_result_deleter<external_async_operator_t>>,
        external_async_error_t>;

    static_assert(detail::context_type_prob_v<external_async_operator_t>,
        "there should be typename external_async_operator_t::context_t which represent the context of the operation.\n"
        "besides, the context_t should be exact a C-struct with trivial destruction.");

    static_assert(detail::context_destroy_ctx_prob_v<external_async_operator_t>,
        "there should be static function whose signature is like:"
        "void external_async_operator_t::destroy_ctx(typename external_async_operator_t::context_t*) noexcept;\n"
        "which is used to destroy the context struct.\n");

    static_assert(detail::submit_prob_v<external_async_operator_t>,
        "there should be static function whose signature is like:"
        "int external_async_operator_t::submit(typename external_async_operator_t::context_t*, external_async_callback_fp_t, external_async_callback_param_t) noexcept;\n"
        "which is used to submit the external async operation.\n");

    static_assert(detail::collect_prob_v<external_async_operator_t>,
        "there should be static function whose signature is like:"
        "typename external_async_operator_t::result_t external_async_operator_t::collect(typename external_async_operator_t::context_t*) noexcept;\n"
        "which is used to collect the result from the context after completion.\n");

    using context_t = typename external_async_operator_t::context_t;

    std::atomic_flag not_ready = ATOMIC_FLAG_INIT;
    context_t ctx{};

#if !FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    bool initialized = false;
#endif

    template<typename param_t>
    explicit external_async_awaitable(param_t&& param)
#if !FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        noexcept
#endif
    {
        not_ready.test_and_set(std::memory_order_relaxed);
        static_assert(detail::context_init_ctx_prob_v<
                external_async_operator_t,
                typename std::decay_t<param_t>::value_type*>,
            "there should be static function whose signature is like:"
            "int external_async_operator_t::init_ctx(typename external_async_operator_t::context_t*, typename std::decay_t<param_t>::value_type*) noexcept;\n"
            "which is used to initialize the context struct.\n");
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        UNLIKELY_IF(param.has_error() || external_async_operator_t::init_ctx(&ctx, std::addressof(param.value()))) {
            throw std::runtime_error("error occurred when initializing external async operation context");
        }
#else
        initialized = param.has_value() && external_async_operator_t::init_ctx(&ctx, std::addressof(param.value())) == 0;
#endif
    }

#if !FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    bool available() noexcept {
        return initialized;
    }
#endif

    external_async_awaitable(const external_async_awaitable&) = delete;
    external_async_awaitable& operator=(const external_async_awaitable&) = delete;
    external_async_awaitable(external_async_awaitable&&) = delete;
    external_async_awaitable& operator=(external_async_awaitable&&) = delete;

    ~external_async_awaitable() noexcept {
#if !FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        if (initialized)
#endif
        {
            external_async_operator_t::destroy_ctx(&ctx);
        }
    }

    static void on_complete(external_async_callback_param_t param) noexcept {
        auto self = static_cast<external_async_awaitable*>(param);
        if (self->not_ready.test_and_set(std::memory_order_acquire)) {
            return;
        }

        auto res = external_async_operator_t::collect(&self->ctx);
        LIKELY_IF(res) {
            self->resume(async_result_type(value_tag, res, external_async_result_deleter<external_async_operator_t>{}));
        } else {
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            async_result_type err(error_tag, nullptr);
            try {
                err.emplace_error(std::make_exception_ptr(std::runtime_error("failed to get external async op result")));
            } catch (...) {
                err.emplace_error(std::current_exception());
            }
            self->resume(std::move(err));
#else
            self->resume(async_result_type(error_tag, std::make_error_code(std::errc::io_error)));
#endif
        }
    }

    int submit() noexcept {
        not_ready.clear(std::memory_order_release);
        return external_async_operator_t::submit(&ctx, on_complete, this);
    }
};

} // namespace extension

template<typename external_async_operator_t>
auto await_external_async() noexcept {
    return await<extension::external_async_awaitable<external_async_operator_t>>();
}

template<typename external_async_operator_t, typename Executor>
auto await_external_async(Executor&& executor_to_resume) noexcept {
    return await<extension::external_async_awaitable<external_async_operator_t>>(std::forward<Executor>(executor_to_resume));
}

} // namespace flux_foundry

#endif
