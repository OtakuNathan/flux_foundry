#include <atomic>
#include <cstdio>
#include <exception>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

#include "flow/flow.h"
#include "extension/external_async_awaitable.h"

using namespace flux_foundry;

namespace {

using err_t = extension::external_async_error_t;

struct inline_executor {
    void dispatch(task_wrapper_sbo t) noexcept {
        t();
    }
};

struct cuda_payload {
    int value{0};
    explicit cuda_payload(int v) noexcept : value(v) {}
};

err_t make_logic_error(const char* msg) noexcept {
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    try {
        throw std::logic_error(msg);
    } catch (...) {
        return std::current_exception();
    }
#else
    (void)msg;
    return std::make_error_code(std::errc::invalid_argument);
#endif
}

bool has_logic_error_message(const err_t& ep, const char* expected) {
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    if (!ep) {
        return false;
    }
    try {
        std::rethrow_exception(ep);
    } catch (const std::exception& e) {
        return std::string(e.what()) == expected;
    } catch (...) {
        return false;
    }
#else
    (void)ep;
    (void)expected;
    return true;
#endif
}

void check(bool cond, const char* name, int& failed) {
    if (cond) {
        std::printf("[OK] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++failed;
    }
}

struct run_observer {
    bool called = false;
    bool has_value = false;
    int value = 0;
    err_t err{};
};

struct mock_cuda_op {
    struct context_t {
        int input = 0;
        cuda_payload* out = nullptr;
    };

    using result_t = cuda_payload*;

    static std::atomic<int> init_count;
    static std::atomic<int> submit_count;
    static std::atomic<int> collect_count;
    static std::atomic<int> destroy_count;
    static std::atomic<int> free_result_count;

    static void reset() noexcept {
        init_count.store(0, std::memory_order_relaxed);
        submit_count.store(0, std::memory_order_relaxed);
        collect_count.store(0, std::memory_order_relaxed);
        destroy_count.store(0, std::memory_order_relaxed);
        free_result_count.store(0, std::memory_order_relaxed);
    }

    static int init_ctx(context_t* ctx, int* in) noexcept {
        init_count.fetch_add(1, std::memory_order_relaxed);
        if (ctx == nullptr || in == nullptr) {
            return -1;
        }
        if (*in < 0) {
            return -1;
        }
        ctx->input = *in;
        return 0;
    }

    static void destroy_ctx(context_t* ctx) noexcept {
        destroy_count.fetch_add(1, std::memory_order_relaxed);
        if (ctx && ctx->out) {
            delete ctx->out;
            ctx->out = nullptr;
        }
    }

    static void free_result(result_t p) noexcept {
        free_result_count.fetch_add(1, std::memory_order_relaxed);
        delete p;
    }

    static int submit(context_t* ctx, extension::external_async_callback_fp_t cb, extension::external_async_callback_param_t user) noexcept {
        submit_count.fetch_add(1, std::memory_order_relaxed);
        if (ctx == nullptr || cb == nullptr || user == nullptr) {
            return -1;
        }
        if (ctx->input == 0) {
            return -1;
        }
        auto* p = new (std::nothrow) cuda_payload(ctx->input + 1);
        if (!p) {
            return -1;
        }
        ctx->out = p;
        cb(user);
        return 0;
    }

    static result_t collect(context_t* ctx) noexcept {
        collect_count.fetch_add(1, std::memory_order_relaxed);
        if (ctx == nullptr) {
            return nullptr;
        }
        auto* p = ctx->out;
        ctx->out = nullptr;
        return p;
    }
};

std::atomic<int> mock_cuda_op::init_count{0};
std::atomic<int> mock_cuda_op::submit_count{0};
std::atomic<int> mock_cuda_op::collect_count{0};
std::atomic<int> mock_cuda_op::destroy_count{0};
std::atomic<int> mock_cuda_op::free_result_count{0};

using awaitable_t = extension::external_async_awaitable<mock_cuda_op>;
using out_t = typename awaitable_t::async_result_type;

struct payload_receiver {
    using value_type = out_t;
    run_observer* obs{};

    void emplace(value_type&& r) noexcept {
        obs->called = true;
        obs->has_value = r.has_value();
        if (r.has_value()) {
            auto& h = r.value();
            obs->value = h ? h->value : -1;
            return;
        }
        obs->err = r.error();
    }
};

int test_cuda_success_path() {
    mock_cuda_op::reset();
    inline_executor ex;
    run_observer obs;

    auto bp = make_blueprint<int>()
        | await_external_async<mock_cuda_op>(&ex)
        | end([](out_t&& in) noexcept -> out_t {
            return std::move(in);
        });

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, payload_receiver{&obs});
    runner(41);

    int failed = 0;
    check(obs.called, "external_async success called", failed);
    check(obs.has_value, "external_async success has_value", failed);
    check(obs.value == 42, "external_async success value == 42", failed);
    check(mock_cuda_op::init_count.load(std::memory_order_relaxed) == 1, "external_async success init_ctx once", failed);
    check(mock_cuda_op::submit_count.load(std::memory_order_relaxed) == 1, "external_async success submit once", failed);
    check(mock_cuda_op::collect_count.load(std::memory_order_relaxed) == 1, "external_async success collect once", failed);
    check(mock_cuda_op::destroy_count.load(std::memory_order_relaxed) == 1, "external_async success destroy_ctx once", failed);
    check(mock_cuda_op::free_result_count.load(std::memory_order_relaxed) == 1, "external_async success free_result once", failed);
    return failed;
}

int test_cuda_init_fail_path() {
    mock_cuda_op::reset();
    inline_executor ex;
    run_observer obs;

    auto bp = make_blueprint<int>()
        | await_external_async<mock_cuda_op>(&ex)
        | end([](out_t&& in) noexcept -> out_t {
            return std::move(in);
        });

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, payload_receiver{&obs});
    runner(-1);

    int failed = 0;
    check(obs.called, "external_async init_fail called", failed);
    check(!obs.has_value, "external_async init_fail has error", failed);
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    check(has_logic_error_message(obs.err, "error occurred when initializing external async operation context"),
          "external_async init_fail error type", failed);
#endif
    check(mock_cuda_op::init_count.load(std::memory_order_relaxed) == 1, "external_async init_fail init_ctx once", failed);
    check(mock_cuda_op::submit_count.load(std::memory_order_relaxed) == 0, "external_async init_fail submit skipped", failed);
    check(mock_cuda_op::collect_count.load(std::memory_order_relaxed) == 0, "external_async init_fail collect skipped", failed);
    check(mock_cuda_op::destroy_count.load(std::memory_order_relaxed) == 0, "external_async init_fail destroy_ctx skipped", failed);
    check(mock_cuda_op::free_result_count.load(std::memory_order_relaxed) == 0, "external_async init_fail free_result skipped", failed);
    return failed;
}

int test_cuda_submit_fail_path() {
    mock_cuda_op::reset();
    inline_executor ex;
    run_observer obs;

    auto bp = make_blueprint<int>()
        | await_external_async<mock_cuda_op>(&ex)
        | end([](out_t&& in) noexcept -> out_t {
            return std::move(in);
        });

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, payload_receiver{&obs});
    runner(0);

    int failed = 0;
    check(obs.called, "external_async submit_fail called", failed);
    check(!obs.has_value, "external_async submit_fail has error", failed);
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    check(has_logic_error_message(obs.err, "failed to submit async operation"),
          "external_async submit_fail async_submission_failed_error", failed);
#endif
    check(mock_cuda_op::init_count.load(std::memory_order_relaxed) == 1, "external_async submit_fail init_ctx once", failed);
    check(mock_cuda_op::submit_count.load(std::memory_order_relaxed) == 1, "external_async submit_fail submit once", failed);
    check(mock_cuda_op::collect_count.load(std::memory_order_relaxed) == 0, "external_async submit_fail collect skipped", failed);
    check(mock_cuda_op::destroy_count.load(std::memory_order_relaxed) == 1, "external_async submit_fail destroy_ctx once", failed);
    check(mock_cuda_op::free_result_count.load(std::memory_order_relaxed) == 0, "external_async submit_fail free_result skipped", failed);
    return failed;
}

} // namespace

int main() {
    int failed = 0;
    failed += test_cuda_success_path();
    failed += test_cuda_init_fail_path();
    failed += test_cuda_submit_fail_path();
    if (failed != 0) {
        std::printf("[FAIL] external_async_awaitable_probe failures=%d\n", failed);
        return 1;
    }
    std::printf("[OK] external_async_awaitable_probe\n");
    return 0;
}
