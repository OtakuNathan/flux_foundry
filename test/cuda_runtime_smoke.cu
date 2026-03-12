#include <atomic>
#include <chrono>
#include <cstdio>
#include <new>
#include <thread>
#include <utility>

#include <cuda_runtime.h>

#include "extension/external_async_awaitable.h"
#include "flow/flow.h"

using namespace flux_foundry;

namespace {

using err_t = extension::external_async_error_t;

struct cuda_add_one_result {
    int value{0};
};

__global__ void add_one_kernel(const int* in, int* out) {
    *out = *in + 1;
}

struct cuda_add_one_async_op {
    struct context_t {
        int input{0};
        int output{0};
        int* d_in{nullptr};
        int* d_out{nullptr};
        cudaStream_t stream{nullptr};
        extension::external_async_callback_fp_t cb{nullptr};
        extension::external_async_callback_param_t user{nullptr};
        int initialized{0};
    };

    using result_t = cuda_add_one_result*;

    static int init_ctx(context_t* ctx, int* in) noexcept {
        if (ctx == nullptr || in == nullptr) {
            return -1;
        }

        ctx->input = *in;
        ctx->output = 0;
        ctx->d_in = nullptr;
        ctx->d_out = nullptr;
        ctx->stream = nullptr;
        ctx->cb = nullptr;
        ctx->user = nullptr;
        ctx->initialized = 0;

        int device_count = 0;
        if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count <= 0) {
            return -1;
        }

        if (cudaStreamCreateWithFlags(&ctx->stream, cudaStreamNonBlocking) != cudaSuccess) {
            return -1;
        }

        if (cudaMalloc(reinterpret_cast<void**>(&ctx->d_in), sizeof(int)) != cudaSuccess) {
            destroy_ctx(ctx);
            return -1;
        }

        if (cudaMalloc(reinterpret_cast<void**>(&ctx->d_out), sizeof(int)) != cudaSuccess) {
            destroy_ctx(ctx);
            return -1;
        }

        ctx->initialized = 1;
        return 0;
    }

    static void destroy_ctx(context_t* ctx) noexcept {
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
            (void) cudaStreamDestroy(ctx->stream);
            ctx->stream = nullptr;
        }
        ctx->initialized = 0;
    }

    static void free_result(result_t p) noexcept {
        delete p;
    }

    static int submit(context_t* ctx, extension::external_async_callback_fp_t cb, extension::external_async_callback_param_t user) noexcept {
        if (ctx == nullptr || ctx->initialized == 0 || cb == nullptr || user == nullptr) {
            return -1;
        }

        ctx->cb = cb;
        ctx->user = user;

        if (cudaMemcpyAsync(ctx->d_in, &ctx->input, sizeof(int), cudaMemcpyHostToDevice, ctx->stream) != cudaSuccess) {
            return -1;
        }

        add_one_kernel<<<1, 1, 0, ctx->stream>>>(ctx->d_in, ctx->d_out);
        if (cudaGetLastError() != cudaSuccess) {
            return -1;
        }

        if (cudaMemcpyAsync(&ctx->output, ctx->d_out, sizeof(int), cudaMemcpyDeviceToHost, ctx->stream) != cudaSuccess) {
            return -1;
        }

        if (cudaLaunchHostFunc(ctx->stream, &on_stream_complete, ctx) != cudaSuccess) {
            return -1;
        }
        return 0;
    }

    static result_t collect(context_t* ctx) noexcept {
        if (ctx == nullptr) {
            return nullptr;
        }
        auto* out = new (std::nothrow) cuda_add_one_result{};
        if (out == nullptr) {
            return nullptr;
        }
        out->value = ctx->output;
        return out;
    }

    static void CUDART_CB on_stream_complete(void* raw) {
        auto* ctx = static_cast<context_t*>(raw);
        if (ctx != nullptr && ctx->cb != nullptr) {
            ctx->cb(ctx->user);
        }
    }
};

using awaitable_t = extension::external_async_awaitable<cuda_add_one_async_op>;
using out_t = typename awaitable_t::async_result_type;

struct run_observer {
    std::atomic<int> done{0};
    std::atomic<int> has_value{0};
    std::atomic<int> value{0};
};

struct receiver {
    using value_type = out_t;
    run_observer* obs{};

    void emplace(value_type&& r) noexcept {
        if (r.has_value() && r.value()) {
            obs->value.store(r.value()->value, std::memory_order_relaxed);
            obs->has_value.store(1, std::memory_order_relaxed);
        } else {
            obs->has_value.store(0, std::memory_order_relaxed);
        }
        obs->done.store(1, std::memory_order_release);
    }
};

bool has_cuda_device() noexcept {
    int device_count = 0;
    return cudaGetDeviceCount(&device_count) == cudaSuccess && device_count > 0;
}

bool wait_done(run_observer& obs, int timeout_ms) noexcept {
    auto begin = std::chrono::steady_clock::now();
    while (obs.done.load(std::memory_order_acquire) == 0) {
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
    if (!has_cuda_device()) {
        std::printf("[SKIP] no CUDA device/runtime available\n");
        return 0;
    }

    run_observer obs;

    auto bp = make_blueprint<int>()
        | await_external_async<cuda_add_one_async_op>()
        | end();

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, receiver{&obs});
    runner(41);

    if (!wait_done(obs, 2000)) {
        std::printf("[FAIL] cuda_runtime_smoke timeout\n");
        return 1;
    }

    if (obs.has_value.load(std::memory_order_acquire) != 1 ||
        obs.value.load(std::memory_order_acquire) != 42) {
        std::printf("[FAIL] cuda_runtime_smoke has_value=%d value=%d\n",
            obs.has_value.load(std::memory_order_relaxed),
            obs.value.load(std::memory_order_relaxed));
        return 1;
    }

    std::printf("[OK] cuda_runtime_smoke value=%d\n", obs.value.load(std::memory_order_relaxed));
    return 0;
}
