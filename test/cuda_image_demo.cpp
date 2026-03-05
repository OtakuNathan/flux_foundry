#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "cuda_image_backend.h"
#include "extension/external_async_awaitable.h"
#include "flow/flow.h"

using namespace flux_foundry;

namespace {

using err_t = extension::external_async_error_t;

struct cuda_image_result {
    int width{0};
    int height{0};
    std::size_t bytes{0};
    unsigned char* rgba{nullptr};
};

struct cuda_image_async_op {
    using context_t = flux_foundry_cuda_image_backend_context;
    using result_t = cuda_image_result*;

    static int init_ctx(context_t* ctx, flux_foundry_cuda_image_render_request* req) noexcept {
        return flux_foundry_cuda_image_backend_init(ctx, req);
    }

    static void destroy_ctx(context_t* ctx) noexcept {
        flux_foundry_cuda_image_backend_destroy(ctx);
    }

    static void free_result(result_t p) noexcept {
        if (p != nullptr) {
            delete[] p->rgba;
            p->rgba = nullptr;
            delete p;
        }
    }

    static int submit(context_t* ctx, extension::external_async_callback_fp_t cb, extension::external_async_callback_param_t user) noexcept {
        return flux_foundry_cuda_image_backend_submit(ctx, cb, user);
    }

    static result_t collect(context_t* ctx) noexcept {
        const unsigned char* src = nullptr;
        std::size_t bytes = 0;
        int width = 0;
        int height = 0;
        if (flux_foundry_cuda_image_backend_get_result(ctx, &src, &bytes, &width, &height) != 0) {
            return nullptr;
        }

        auto* out = new (std::nothrow) cuda_image_result{};
        if (out == nullptr) {
            return nullptr;
        }

        auto* rgba = new (std::nothrow) unsigned char[bytes];
        if (rgba == nullptr) {
            delete out;
            return nullptr;
        }

        std::memcpy(rgba, src, bytes);
        out->width = width;
        out->height = height;
        out->bytes = bytes;
        out->rgba = rgba;
        return out;
    }
};

using awaitable_t = extension::external_async_awaitable<cuda_image_async_op>;
using image_out_t = typename awaitable_t::async_result_type;
using path_out_t = result_t<std::string, err_t>;

err_t make_error(const char* msg) noexcept {
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    try {
        throw std::runtime_error(msg);
    } catch (...) {
        return std::current_exception();
    }
#else
    (void)msg;
    return std::make_error_code(std::errc::io_error);
#endif
}

bool write_ppm(const char* path, const cuda_image_result& image) noexcept {
    if (path == nullptr || image.width <= 0 || image.height <= 0 || image.rgba == nullptr) {
        return false;
    }

    FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        return false;
    }

    const int header_written = std::fprintf(f, "P6\n%d %d\n255\n", image.width, image.height);
    if (header_written <= 0) {
        std::fclose(f);
        return false;
    }

    const std::size_t pixel_count = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
    auto rgb = std::unique_ptr<unsigned char[]>(new (std::nothrow) unsigned char[pixel_count * 3u]);
    if (!rgb) {
        std::fclose(f);
        return false;
    }

    for (std::size_t i = 0; i < pixel_count; ++i) {
        rgb[i * 3u + 0u] = image.rgba[i * 4u + 0u];
        rgb[i * 3u + 1u] = image.rgba[i * 4u + 1u];
        rgb[i * 3u + 2u] = image.rgba[i * 4u + 2u];
    }

    const std::size_t wrote = std::fwrite(rgb.get(), 1, pixel_count * 3u, f);
    std::fclose(f);
    return wrote == pixel_count * 3u;
}

void maybe_open_image(const std::string& path) noexcept {
    const char* no_open = std::getenv("FLUX_FOUNDRY_CUDA_DEMO_NO_OPEN");
    if (no_open != nullptr && no_open[0] != '\0' && no_open[0] != '0') {
        return;
    }

#if defined(_WIN32)
    std::string cmd = "start \"\" \"" + path + "\"";
#elif defined(__APPLE__)
    std::string cmd = "open \"" + path + "\"";
#else
    std::string cmd = "xdg-open \"" + path + "\" >/dev/null 2>&1";
#endif

    (void)std::system(cmd.c_str());
}

struct demo_state {
    std::atomic<int> done{0};
};

struct demo_receiver {
    using value_type = path_out_t;
    demo_state* state{};

    void emplace(value_type&& r) noexcept {
        if (r.has_value()) {
            std::printf("[OK] wrote image: %s\n", r.value().c_str());
            maybe_open_image(r.value());
        } else {
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            try {
                std::rethrow_exception(r.error());
            } catch (const std::exception& e) {
                std::printf("[FAIL] cuda_image_demo: %s\n", e.what());
            } catch (...) {
                std::printf("[FAIL] cuda_image_demo: unknown error\n");
            }
#else
            std::printf("[FAIL] cuda_image_demo: error_code=%d\n", static_cast<int>(r.error().value()));
#endif
        }
        state->done.store(1, std::memory_order_release);
    }
};

bool wait_done(demo_state& state, int timeout_ms) noexcept {
    auto begin = std::chrono::steady_clock::now();
    while (state.done.load(std::memory_order_acquire) == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        auto elapsed = std::chrono::steady_clock::now() - begin;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > timeout_ms) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    if (flux_foundry_cuda_image_backend_has_device() == 0) {
        std::printf("[SKIP] no CUDA device/runtime available\n");
        return 0;
    }

    demo_state state;
    const char* output_path = "cuda_flow_demo.ppm";

    auto bp = make_blueprint<flux_foundry_cuda_image_render_request, err_t>()
        | await_external_async<cuda_image_async_op>()
        | then([output_path](image_out_t&& in) -> path_out_t {
            if (in.has_error()) {
                return path_out_t(error_tag, std::move(in).error());
            }

            auto img = std::move(in).value();
            if (!img) {
                return path_out_t(error_tag, make_error("cuda image collect returned null"));
            }

            if (!write_ppm(output_path, *img)) {
                return path_out_t(error_tag, make_error("failed to write PPM image"));
            }

            return path_out_t(value_tag, std::string(output_path));
        })
        | end();

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, demo_receiver{&state});
    runner(flux_foundry_cuda_image_render_request{1024, 768, 0});

    if (!wait_done(state, 8000)) {
        std::printf("[FAIL] cuda_image_demo timeout\n");
        return 1;
    }

    return 0;
}

