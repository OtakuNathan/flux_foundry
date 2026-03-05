#ifndef FLUX_FOUNDRY_TEST_CUDA_IMAGE_BACKEND_H
#define FLUX_FOUNDRY_TEST_CUDA_IMAGE_BACKEND_H

#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*flux_foundry_cuda_callback_t)(void*);

struct flux_foundry_cuda_image_render_request {
    int width;
    int height;
    int frame;
};

struct flux_foundry_cuda_image_backend_context {
    int width;
    int height;
    int frame;
    unsigned char* d_rgba;
    unsigned char* h_rgba;
    std::size_t bytes;
    void* stream;
    flux_foundry_cuda_callback_t cb;
    void* user;
    int initialized;
};

int flux_foundry_cuda_image_backend_has_device() noexcept;
int flux_foundry_cuda_image_backend_init(flux_foundry_cuda_image_backend_context* ctx,
    const flux_foundry_cuda_image_render_request* req) noexcept;
void flux_foundry_cuda_image_backend_destroy(flux_foundry_cuda_image_backend_context* ctx) noexcept;
int flux_foundry_cuda_image_backend_submit(flux_foundry_cuda_image_backend_context* ctx,
    flux_foundry_cuda_callback_t cb,
    void* user) noexcept;
int flux_foundry_cuda_image_backend_get_result(flux_foundry_cuda_image_backend_context* ctx,
    const unsigned char** pixels,
    std::size_t* bytes,
    int* width,
    int* height) noexcept;

#ifdef __cplusplus
}
#endif

#endif
