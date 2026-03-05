#include "cuda_runtime_backend.h"

#include <cuda_runtime.h>

namespace {

__global__ void add_one_kernel(const int* in, int* out) {
    *out = *in + 1;
}

void CUDART_CB on_stream_complete(void* raw) {
    auto* ctx = static_cast<flux_foundry_cuda_backend_context*>(raw);
    if (ctx != nullptr && ctx->cb != nullptr) {
        ctx->cb(ctx->user);
    }
}

cudaStream_t to_stream(void* s) {
    return reinterpret_cast<cudaStream_t>(s);
}

void* from_stream(cudaStream_t s) {
    return reinterpret_cast<void*>(s);
}

} // namespace

extern "C" {

int flux_foundry_cuda_backend_has_device() noexcept {
    int device_count = 0;
    return (cudaGetDeviceCount(&device_count) == cudaSuccess && device_count > 0) ? 1 : 0;
}

int flux_foundry_cuda_backend_init(flux_foundry_cuda_backend_context* ctx, int input) noexcept {
    if (ctx == nullptr) {
        return -1;
    }

    ctx->input = input;
    ctx->output = 0;
    ctx->d_in = nullptr;
    ctx->d_out = nullptr;
    ctx->stream = nullptr;
    ctx->cb = nullptr;
    ctx->user = nullptr;
    ctx->initialized = 0;

    if (!flux_foundry_cuda_backend_has_device()) {
        return -1;
    }

    cudaStream_t stream = nullptr;
    if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess) {
        return -1;
    }
    ctx->stream = from_stream(stream);

    if (cudaMalloc(reinterpret_cast<void**>(&ctx->d_in), sizeof(int)) != cudaSuccess) {
        flux_foundry_cuda_backend_destroy(ctx);
        return -1;
    }

    if (cudaMalloc(reinterpret_cast<void**>(&ctx->d_out), sizeof(int)) != cudaSuccess) {
        flux_foundry_cuda_backend_destroy(ctx);
        return -1;
    }

    ctx->initialized = 1;
    return 0;
}

void flux_foundry_cuda_backend_destroy(flux_foundry_cuda_backend_context* ctx) noexcept {
    if (ctx == nullptr) {
        return;
    }

    if (ctx->d_in != nullptr) {
        (void) cudaFree(ctx->d_in);
        ctx->d_in = nullptr;
    }
    if (ctx->d_out != nullptr) {
        (void) cudaFree(ctx->d_out);
        ctx->d_out = nullptr;
    }
    if (ctx->stream != nullptr) {
        (void) cudaStreamDestroy(to_stream(ctx->stream));
        ctx->stream = nullptr;
    }

    ctx->initialized = 0;
}

int flux_foundry_cuda_backend_submit(flux_foundry_cuda_backend_context* ctx, flux_foundry_cuda_callback_t cb, void* user) noexcept {
    if (ctx == nullptr || ctx->initialized == 0 || cb == nullptr || user == nullptr) {
        return -1;
    }

    cudaStream_t stream = to_stream(ctx->stream);
    ctx->cb = cb;
    ctx->user = user;

    if (cudaMemcpyAsync(ctx->d_in, &ctx->input, sizeof(int), cudaMemcpyHostToDevice, stream) != cudaSuccess) {
        return -1;
    }

    add_one_kernel<<<1, 1, 0, stream>>>(ctx->d_in, ctx->d_out);
    if (cudaGetLastError() != cudaSuccess) {
        return -1;
    }

    if (cudaMemcpyAsync(&ctx->output, ctx->d_out, sizeof(int), cudaMemcpyDeviceToHost, stream) != cudaSuccess) {
        return -1;
    }

    if (cudaLaunchHostFunc(stream, &on_stream_complete, ctx) != cudaSuccess) {
        return -1;
    }

    return 0;
}

int flux_foundry_cuda_backend_collect(flux_foundry_cuda_backend_context* ctx, int* output) noexcept {
    if (ctx == nullptr || output == nullptr || ctx->initialized == 0) {
        return -1;
    }

    *output = ctx->output;
    return 0;
}

} // extern "C"
