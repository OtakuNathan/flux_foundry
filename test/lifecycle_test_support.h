#ifndef FLUX_FOUNDRY_LIFECYCLE_TEST_SUPPORT_H
#define FLUX_FOUNDRY_LIFECYCLE_TEST_SUPPORT_H
#include <system_error>
#include "base/traits.h"
#include "flow/flow_def.h"
#if !FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
namespace flux_foundry {
template <> struct cancel_error<std::error_code> {
    static std::error_code make(cancel_kind k) noexcept { return {k == cancel_kind::hard ? 1 : 2, std::generic_category()}; }
};
template <> struct awaitable_creating_error<std::error_code> {
    static std::error_code make() noexcept { return {3, std::generic_category()}; }
};
template <> struct async_submission_failed_error<std::error_code> {
    static std::error_code make() noexcept { return {4, std::generic_category()}; }
};
template <> struct async_all_failed_error<std::error_code> {
    static std::error_code make() noexcept { return {5, std::generic_category()}; }
};
template <> struct async_any_failed_error<std::error_code> {
    static std::error_code make(size_t i) noexcept { return {static_cast<int>(6 + i), std::generic_category()}; }
};
}
#endif
#include "flow/flow.h"
namespace lifecycle_test {
using error = flux_foundry::flow_async_agg_err_t;
inline error failure() noexcept { return flux_foundry::async_submission_failed_error<error>::make(); }
}
#endif
