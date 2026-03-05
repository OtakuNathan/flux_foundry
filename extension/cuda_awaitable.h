#ifndef FLUX_FOUNDRY_CUDA_AWAITABLE_H
#define FLUX_FOUNDRY_CUDA_AWAITABLE_H

#include <utility>

#include "external_async_awaitable.h"

namespace flux_foundry {
namespace extension {

using cuda_error_t = external_async_error_t;
using cuda_callback_fp_t = external_async_callback_fp_t;
using cuda_callback_param_t = external_async_callback_param_t;

template<typename cuda_operator_t>
using cuda_awaitable = external_async_awaitable<cuda_operator_t>;

template<typename cuda_operator_t>
using cuda_result_deleter = external_async_result_deleter<cuda_operator_t>;

} // namespace extension

template<typename cuda_operator_t>
auto await_cuda() noexcept {
    return await_external_async<cuda_operator_t>();
}

template<typename cuda_operator_t, typename Executor>
auto await_cuda(Executor&& executor_to_resume) noexcept {
    return await_external_async<cuda_operator_t>(std::forward<Executor>(executor_to_resume));
}

} // namespace flux_foundry

#endif