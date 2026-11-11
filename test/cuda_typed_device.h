#pragma once
#include <cuda_runtime_api.h>
#include <vector>

// The native C++ device boundary carries owned results and per-operation state.
// Neither FF nor Portal templates are compiled by NVCC.
struct image_result { int width, height; std::vector<unsigned char> pixels; };
namespace typed_cuda {
struct resources {
    cudaStream_t stream = nullptr;
    cudaEvent_t event = nullptr;
    void* device = nullptr;
    std::vector<unsigned char> host;
    cudaError_t error = cudaSuccess;
    bool recorded = false;
    ~resources() noexcept;
    resources() = default;
    resources(const resources&) = delete;
    resources& operator=(const resources&) = delete;
};
void launch_add_one(resources&, int value) noexcept;
void launch_render(resources&, int width, int height) noexcept;
}
