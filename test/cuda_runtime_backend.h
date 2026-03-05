#ifndef FLUX_FOUNDRY_TEST_CUDA_RUNTIME_BACKEND_H
#define FLUX_FOUNDRY_TEST_CUDA_RUNTIME_BACKEND_H

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*flux_foundry_cuda_callback_t)(void*);

struct flux_foundry_cuda_backend_context {
    int input;
    int output;
    int* d_in;
    int* d_out;
    void* stream;
    flux_foundry_cuda_callback_t cb;
    void* user;
    int initialized;
};

int flux_foundry_cuda_backend_has_device() noexcept;
int flux_foundry_cuda_backend_init(flux_foundry_cuda_backend_context* ctx, int input) noexcept;
void flux_foundry_cuda_backend_destroy(flux_foundry_cuda_backend_context* ctx) noexcept;
int flux_foundry_cuda_backend_submit(flux_foundry_cuda_backend_context* ctx, flux_foundry_cuda_callback_t cb, void* user) noexcept;
int flux_foundry_cuda_backend_collect(flux_foundry_cuda_backend_context* ctx, int* output) noexcept;

#ifdef __cplusplus
}
#endif

#endif
