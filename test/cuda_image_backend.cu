#include "cuda_image_backend.h"

#include <cuda_runtime.h>

namespace {

__global__ void mandelbrot_rgba_kernel(unsigned char* rgba, int width, int height, float scale) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }

    const float nx = (2.0f * x - width) / static_cast<float>(height);
    const float ny = (2.0f * y - height) / static_cast<float>(height);

    const float cx = -0.743643887037151f + nx * 1.3f * scale;
    const float cy = 0.131825904205330f + ny * 1.3f * scale;

    float zx = 0.0f;
    float zy = 0.0f;
    int iter = 0;
    constexpr int k_max_iter = 300;
    while (zx * zx + zy * zy <= 4.0f && iter < k_max_iter) {
        const float x2 = zx * zx - zy * zy + cx;
        zy = 2.0f * zx * zy + cy;
        zx = x2;
        ++iter;
    }

    float t = iter / static_cast<float>(k_max_iter);
    if (iter == k_max_iter) {
        t = 0.0f;
    }

    const unsigned char r = static_cast<unsigned char>(9.0f * (1.0f - t) * t * t * t * 255.0f);
    const unsigned char g = static_cast<unsigned char>(15.0f * (1.0f - t) * (1.0f - t) * t * t * 255.0f);
    const unsigned char b = static_cast<unsigned char>(8.5f * (1.0f - t) * (1.0f - t) * (1.0f - t) * t * 255.0f);

    const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
    const std::size_t base = i * 4u;
    rgba[base + 0u] = r;
    rgba[base + 1u] = g;
    rgba[base + 2u] = b;
    rgba[base + 3u] = 255u;
}

void CUDART_CB on_stream_complete(void* raw) {
    auto* ctx = static_cast<flux_foundry_cuda_image_backend_context*>(raw);
    if (ctx != nullptr && ctx->cb != nullptr) {
        ctx->cb(ctx->user);
    }
}

inline cudaStream_t to_stream(void* s) {
    return reinterpret_cast<cudaStream_t>(s);
}

inline void* from_stream(cudaStream_t s) {
    return reinterpret_cast<void*>(s);
}

} // namespace

extern "C" {

int flux_foundry_cuda_image_backend_has_device() noexcept {
    int device_count = 0;
    return (cudaGetDeviceCount(&device_count) == cudaSuccess && device_count > 0) ? 1 : 0;
}

int flux_foundry_cuda_image_backend_init(flux_foundry_cuda_image_backend_context* ctx,
    const flux_foundry_cuda_image_render_request* req) noexcept {
    if (ctx == nullptr || req == nullptr || req->width <= 0 || req->height <= 0) {
        return -1;
    }

    ctx->width = req->width;
    ctx->height = req->height;
    ctx->frame = req->frame;
    ctx->d_rgba = nullptr;
    ctx->h_rgba = nullptr;
    ctx->bytes = 0;
    ctx->stream = nullptr;
    ctx->cb = nullptr;
    ctx->user = nullptr;
    ctx->initialized = 0;

    if (!flux_foundry_cuda_image_backend_has_device()) {
        return -1;
    }

    const std::size_t pixel_count = static_cast<std::size_t>(ctx->width) * static_cast<std::size_t>(ctx->height);
    ctx->bytes = pixel_count * 4u;

    cudaStream_t stream = nullptr;
    if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess) {
        return -1;
    }
    ctx->stream = from_stream(stream);

    if (cudaMalloc(reinterpret_cast<void**>(&ctx->d_rgba), ctx->bytes) != cudaSuccess) {
        flux_foundry_cuda_image_backend_destroy(ctx);
        return -1;
    }

    if (cudaMallocHost(reinterpret_cast<void**>(&ctx->h_rgba), ctx->bytes) != cudaSuccess) {
        flux_foundry_cuda_image_backend_destroy(ctx);
        return -1;
    }

    ctx->initialized = 1;
    return 0;
}

void flux_foundry_cuda_image_backend_destroy(flux_foundry_cuda_image_backend_context* ctx) noexcept {
    if (ctx == nullptr) {
        return;
    }

    if (ctx->d_rgba != nullptr) {
        (void)cudaFree(ctx->d_rgba);
        ctx->d_rgba = nullptr;
    }

    if (ctx->h_rgba != nullptr) {
        (void)cudaFreeHost(ctx->h_rgba);
        ctx->h_rgba = nullptr;
    }

    if (ctx->stream != nullptr) {
        (void)cudaStreamDestroy(to_stream(ctx->stream));
        ctx->stream = nullptr;
    }

    ctx->bytes = 0;
    ctx->initialized = 0;
}

int flux_foundry_cuda_image_backend_submit(flux_foundry_cuda_image_backend_context* ctx,
    flux_foundry_cuda_callback_t cb,
    void* user) noexcept {
    if (ctx == nullptr || ctx->initialized == 0 || cb == nullptr || user == nullptr) {
        return -1;
    }

    ctx->cb = cb;
    ctx->user = user;

    cudaStream_t stream = to_stream(ctx->stream);
    const dim3 block(16, 16);
    const dim3 grid(
        static_cast<unsigned int>((ctx->width + static_cast<int>(block.x) - 1) / static_cast<int>(block.x)),
        static_cast<unsigned int>((ctx->height + static_cast<int>(block.y) - 1) / static_cast<int>(block.y)));

    float scale = 1.0f;
    for (int i = 0; i < ctx->frame; ++i) {
        scale *= 0.975f;
    }

    mandelbrot_rgba_kernel<<<grid, block, 0, stream>>>(ctx->d_rgba, ctx->width, ctx->height, scale);
    if (cudaGetLastError() != cudaSuccess) {
        return -1;
    }

    if (cudaMemcpyAsync(ctx->h_rgba, ctx->d_rgba, ctx->bytes, cudaMemcpyDeviceToHost, stream) != cudaSuccess) {
        return -1;
    }

    if (cudaLaunchHostFunc(stream, &on_stream_complete, ctx) != cudaSuccess) {
        return -1;
    }

    return 0;
}

int flux_foundry_cuda_image_backend_get_result(flux_foundry_cuda_image_backend_context* ctx,
    const unsigned char** pixels,
    std::size_t* bytes,
    int* width,
    int* height) noexcept {
    if (ctx == nullptr || ctx->initialized == 0 || pixels == nullptr || bytes == nullptr || width == nullptr || height == nullptr) {
        return -1;
    }

    *pixels = ctx->h_rgba;
    *bytes = ctx->bytes;
    *width = ctx->width;
    *height = ctx->height;
    return 0;
}

} // extern "C"
