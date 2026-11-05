#pragma once
#include <cuda_runtime.h>
#include <cstddef>
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

}
