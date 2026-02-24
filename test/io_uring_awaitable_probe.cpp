#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#include "base/traits.h"
#include "flow/flow_def.h"

namespace flux_foundry {
template <>
struct cancel_error<std::error_code> {
    static std::error_code make(cancel_kind kind) noexcept {
        return std::error_code(kind == cancel_kind::hard ? 1001 : 1002, std::generic_category());
    }
};

template <>
struct awaitable_creating_error<std::error_code> {
    static std::error_code make() noexcept {
        return std::error_code(1003, std::generic_category());
    }
};

template <>
struct async_submission_failed_error<std::error_code> {
    static std::error_code make() noexcept {
        return std::error_code(1004, std::generic_category());
    }
};

template <>
struct async_all_failed_error<std::error_code> {
    static std::error_code make() noexcept {
        return std::error_code(1005, std::generic_category());
    }
};

template <>
struct async_any_failed_error<std::error_code> {
    static std::error_code make(size_t i) noexcept {
        return std::error_code(static_cast<int>(1100 + i), std::generic_category());
    }
};
} // namespace flux_foundry

#include "executor/simple_executor.h"
#include "flow/flow.h"

#if defined(__linux__) && __has_include(<liburing.h>)
#define FLUX_FOUNDRY_TEST_HAS_LIBURING 1
#include <liburing.h>
#else
#define FLUX_FOUNDRY_TEST_HAS_LIBURING 0
#endif

using namespace flux_foundry;

namespace {

using err_t = std::error_code;
using out_t = result_t<int, err_t>;

void check(bool cond, const char* name, int& failed) {
    if (cond) {
        std::printf("[OK] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++failed;
    }
}

struct run_observer {
    std::atomic<int> done{0};
    std::atomic<int> has_value{-1};
    std::atomic<int> value{0};
    std::atomic<int> err_code{0};
};

struct int_receiver {
    using value_type = out_t;
    run_observer* obs;

    void emplace(value_type&& r) noexcept {
        if (r.has_value()) {
            obs->value.store(r.value(), std::memory_order_relaxed);
            obs->has_value.store(1, std::memory_order_relaxed);
        } else {
            obs->err_code.store(r.error().value(), std::memory_order_relaxed);
            obs->has_value.store(0, std::memory_order_relaxed);
        }
        obs->done.store(1, std::memory_order_release);
    }
};

bool wait_done(run_observer& obs, int timeout_ms) {
    auto begin = std::chrono::steady_clock::now();
    while (obs.done.load(std::memory_order_acquire) == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        auto elapsed = std::chrono::steady_clock::now() - begin;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > timeout_ms) {
            return false;
        }
    }
    return true;
}

struct executor_env {
    simple_executor<1024> ex;
    std::thread worker;

    executor_env()
        : worker([this]() noexcept { ex.run(); }) {
        std::atomic<int> started{0};
        ex.dispatch(task_wrapper_sbo([&started]() noexcept {
            started.store(1, std::memory_order_release);
        }));

        auto begin = std::chrono::steady_clock::now();
        while (started.load(std::memory_order_acquire) == 0) {
            std::this_thread::yield();
            auto elapsed = std::chrono::steady_clock::now() - begin;
            if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() > 2) {
                std::printf("[FAIL] executor start timeout\n");
                std::abort();
            }
        }
    }

    ~executor_env() {
        bool shutdown_ok = false;
        for (int i = 0; i < 200000; ++i) {
            if (ex.try_shutdown()) {
                shutdown_ok = true;
                break;
            }
            std::this_thread::yield();
        }
        if (!shutdown_ok) {
            std::printf("[FAIL] executor shutdown timeout\n");
            std::abort();
        }
        if (worker.joinable()) {
            worker.join();
        }
    }
};

#if FLUX_FOUNDRY_TEST_HAS_LIBURING

struct uring_nop_awaitable;

struct uring_runtime {
    static constexpr uintptr_t k_stop_tag = static_cast<uintptr_t>(1);

    io_uring ring{};
    std::mutex submit_mu;
    std::thread cq_thread;
    std::atomic<int> stopping{0};
    std::atomic<int> ok_{0};

    uring_runtime(unsigned entries = 256) {
        int rc = io_uring_queue_init(entries, &ring, 0);
        if (rc < 0) {
            std::printf("[SKIP] io_uring_queue_init failed: %d\n", rc);
            return;
        }

        ok_.store(1, std::memory_order_release);
        cq_thread = std::thread([this]() noexcept { cq_loop(); });
    }

    ~uring_runtime() {
        shutdown();
    }

    bool ok() const noexcept {
        return ok_.load(std::memory_order_acquire) == 1;
    }

    int submit_nop(uring_nop_awaitable* aw) noexcept;

    void shutdown() noexcept {
        if (!ok()) {
            return;
        }

        int expected = 0;
        if (!stopping.compare_exchange_strong(expected, 1,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (cq_thread.joinable()) {
                cq_thread.join();
            }
            io_uring_queue_exit(&ring);
            ok_.store(0, std::memory_order_release);
            return;
        }

        {
            std::lock_guard<std::mutex> lk(submit_mu);
            io_uring_sqe* sqe = io_uring_get_sqe(&ring);
            if (sqe != nullptr) {
                io_uring_prep_nop(sqe);
                io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(k_stop_tag));
                (void) io_uring_submit(&ring);
            }
        }

        if (cq_thread.joinable()) {
            cq_thread.join();
        }
        io_uring_queue_exit(&ring);
        ok_.store(0, std::memory_order_release);
    }

private:
    void cq_loop() noexcept;
};

static uring_runtime* g_uring = nullptr;

struct uring_nop_awaitable final : awaitable_base<uring_nop_awaitable, int, err_t> {
    using async_result_type = out_t;
    constexpr static bool support_cancel = false;

    uring_runtime* rt{nullptr};
    int v{0};

    explicit uring_nop_awaitable(async_result_type&& in) noexcept
        : rt(g_uring),
          v(in.has_value() ? in.value() : 0) {
    }

    bool available() const noexcept {
        return rt != nullptr && rt->ok();
    }

    int submit() noexcept {
        if (rt == nullptr || !rt->ok()) {
            return -1;
        }

        // Backend completion holds a reference until CQE callback finishes.
        this->retain();
        int rc = rt->submit_nop(this);
        if (rc != 0) {
            this->release();
            return rc;
        }
        return 0;
    }

    void cancel() noexcept {
        // MVP probe: cancel is intentionally disabled (support_cancel=false).
    }

    void on_cqe(int res) noexcept {
        if (res >= 0) {
            this->resume(async_result_type(value_tag, v + 1));
        } else {
            this->resume(async_result_type(error_tag, std::error_code(-res, std::generic_category())));
        }
        this->release();
    }
};

int uring_runtime::submit_nop(uring_nop_awaitable* aw) noexcept {
    if (!ok() || stopping.load(std::memory_order_acquire) != 0) {
        return -1;
    }

    std::lock_guard<std::mutex> lk(submit_mu);
    io_uring_sqe* sqe = io_uring_get_sqe(&ring);
    if (sqe == nullptr) {
        return -1;
    }

    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data(sqe, aw);
    int rc = io_uring_submit(&ring);
    if (rc < 0) {
        return rc;
    }
    if (rc == 0) {
        return -1;
    }
    return 0;
}

void uring_runtime::cq_loop() noexcept {
    for (;;) {
        io_uring_cqe* cqe = nullptr;
        int rc = io_uring_wait_cqe(&ring, &cqe);
        if (rc < 0) {
            if (stopping.load(std::memory_order_acquire) != 0) {
                return;
            }
            continue;
        }

        void* tag = io_uring_cqe_get_data(cqe);
        int res = cqe->res;
        io_uring_cqe_seen(&ring, cqe);

        if (reinterpret_cast<uintptr_t>(tag) == k_stop_tag) {
            return;
        }

        if (tag != nullptr) {
            static_cast<uring_nop_awaitable*>(tag)->on_cqe(res);
        }
    }
}

int test_uring_nop_chain_success() {
    uring_runtime ring;
    if (!ring.ok()) {
        return 0;
    }
    g_uring = &ring;

    executor_env env;
    run_observer obs;

    auto bp = make_blueprint<int, err_t>()
        | await<uring_nop_awaitable>(&env.ex)
        | await<uring_nop_awaitable>(&env.ex)
        | end();

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, int_receiver{&obs});
    runner(5);

    int failed = 0;
    check(wait_done(obs, 1000), "io_uring nop chain wait done", failed);
    check(obs.has_value.load(std::memory_order_acquire) == 1, "io_uring nop chain has value", failed);
    check(obs.value.load(std::memory_order_acquire) == 7, "io_uring nop chain value == 7", failed);

    g_uring = nullptr;
    return failed;
}

int test_uring_creation_fail_without_runtime() {
    g_uring = nullptr;
    executor_env env;
    run_observer obs;

    auto bp = make_blueprint<int, err_t>()
        | await<uring_nop_awaitable>(&env.ex)
        | end();

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(bp_ptr, int_receiver{&obs});
    runner(1);

    int failed = 0;
    check(wait_done(obs, 1000), "io_uring create_fail wait done", failed);
    check(obs.has_value.load(std::memory_order_acquire) == 0, "io_uring create_fail has error", failed);
    check(obs.err_code.load(std::memory_order_acquire) == 1003, "io_uring create_fail awaitable_creating_error", failed);
    return failed;
}

#endif // FLUX_FOUNDRY_TEST_HAS_LIBURING

} // namespace

int main() {
#if !FLUX_FOUNDRY_TEST_HAS_LIBURING
    std::printf("[SKIP] io_uring probe requires Linux + liburing headers\n");
    return 0;
#else
    int failed = 0;
    failed += test_uring_nop_chain_success();
    failed += test_uring_creation_fail_without_runtime();

    if (failed == 0) {
        std::printf("[PASS] io_uring awaitable probe passed\n");
        return 0;
    }
    std::printf("[FAIL] io_uring awaitable probe failed=%d\n", failed);
    return 1;
#endif
}
