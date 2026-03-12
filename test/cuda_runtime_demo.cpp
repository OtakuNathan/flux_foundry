#include <atomic>
#include <chrono>
#include <cstdio>
#include <new>
#include <thread>
#include <utility>

#include "cuda_runtime_backend.h"
#include "extension/external_async_awaitable.h"
#include "flow/flow.h"

using namespace flux_foundry;

namespace {

struct cuda_add_one_result {
    int value{0};
};

struct cuda_add_one_async_op {
    using context_t = flux_foundry_cuda_backend_context;
    using result_t = cuda_add_one_result*;

    static int init_ctx(context_t* ctx, int* in) noexcept {
        if (in == nullptr) {
            return -1;
        }
        return flux_foundry_cuda_backend_init(ctx, *in);
    }

    static void destroy_ctx(context_t* ctx) noexcept {
        flux_foundry_cuda_backend_destroy(ctx);
    }

    static void free_result(result_t p) noexcept {
        delete p;
    }

    static int submit(context_t* ctx, extension::external_async_callback_fp_t cb, extension::external_async_callback_param_t user) noexcept {
        return flux_foundry_cuda_backend_submit(ctx, cb, user);
    }

    static result_t collect(context_t* ctx) noexcept {
        int output = 0;
        if (flux_foundry_cuda_backend_collect(ctx, &output) != 0) {
            return nullptr;
        }

        auto* out = new (std::nothrow) cuda_add_one_result{};
        if (out == nullptr) {
            return nullptr;
        }
        out->value = output;
        return out;
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
    if (flux_foundry_cuda_backend_has_device() == 0) {
        std::printf("[SKIP] no CUDA device/runtime available\n");
        return 0;
    }

    run_observer obs;

    auto bp = make_blueprint<int>()
        | await_external_async<cuda_add_one_async_op>()
        | end([](out_t&& in) noexcept -> out_t {
            return std::move(in);
        });

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, receiver{&obs});
    runner(41);

    if (!wait_done(obs, 2000)) {
        std::printf("[FAIL] cuda_runtime_demo timeout\n");
        return 1;
    }

    if (obs.has_value.load(std::memory_order_acquire) != 1 ||
        obs.value.load(std::memory_order_acquire) != 42) {
        std::printf("[FAIL] cuda_runtime_demo has_value=%d value=%d\n",
            obs.has_value.load(std::memory_order_relaxed),
            obs.value.load(std::memory_order_relaxed));
        return 1;
    }

    std::printf("[OK] cuda_runtime_demo value=%d\n", obs.value.load(std::memory_order_relaxed));
    return 0;
}
