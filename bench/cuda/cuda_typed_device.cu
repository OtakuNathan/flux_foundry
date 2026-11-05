#include "cuda_typed_device.h"
#include "cuda_image_kernel.cuh"
namespace {
__global__ void add_one_kernel(int value, int* output) { *output = value + 1; }
}
namespace typed_cuda {
resources::~resources() noexcept {
    if (device) cudaFree(device);
    if (event) cudaEventDestroy(event);
    if (stream) cudaStreamDestroy(stream);
}
void launch_add_one(resources& r, int value) noexcept {
    add_one_kernel<<<1, 1, 0, r.stream>>>(value, static_cast<int*>(r.device));
}
void launch_render(resources& r, int width, int height) noexcept {
    mandelbrot_rgba_kernel<<<dim3((width + 15) / 16, (height + 15) / 16), dim3(16, 16), 0, r.stream>>>(
        static_cast<unsigned char*>(r.device), width, height, 1.f);
}
}
