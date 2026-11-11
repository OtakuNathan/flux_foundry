#include "executor/gsource_executor.h"
#include "memory/lite_ptr.h"
#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

using namespace flux_foundry;

template <size_t Capacity>
bool exercise() {
    auto* context = g_main_context_new();
    bool ok = true;
    {
        gsource_executor<Capacity> ex;
        ok = ex.register_to(context) == 0;
        int delivered = 0;
        // Dispatch from another source on the same context, not just our source.
        // A full queue must not block its own event-loop thread.
        g_main_context_acquire(context);
        for (size_t i = 0; i < Capacity + 3; ++i) {
            ex.dispatch(task_wrapper_sbo([&]() noexcept { ++delivered; }));
        }
        g_main_context_release(context);
        while (delivered != static_cast<int>(Capacity + 3)) {
            g_main_context_iteration(context, TRUE);
        }
        ex.dispatch(task_wrapper_sbo([&]() noexcept {
            for (size_t i = 0; i < Capacity + 3; ++i) {
                ex.dispatch(task_wrapper_sbo([&]() noexcept { ++delivered; }));
            }
        }));
        while (delivered != 2 * static_cast<int>(Capacity + 3)) {
            g_main_context_iteration(context, TRUE);
        }
        std::thread producer([&] {
            for (int i = 0; i < 100; ++i) {
                ex.dispatch(task_wrapper_sbo([&]() noexcept { ++delivered; }));
            }
        });
        while (delivered != 2 * static_cast<int>(Capacity + 3) + 100) {
            g_main_context_iteration(context, TRUE);
        }
        producer.join();
    }
    g_main_context_unref(context);
    return ok;
}

bool destruction(bool nested) {
    auto* context = g_main_context_new();
    auto ex = make_lite_ptr<gsource_executor<16>>();
    bool ok = ex->register_to(nullptr) == EINVAL && ex->register_to(context) == 0 &&
        ex->register_to(context) == EALREADY;
    int calls = 0;
    auto token = std::make_shared<int>(42);
    std::weak_ptr<int> observer = token;
    auto enqueue_release = [&]() noexcept {
        ex->dispatch(task_wrapper_sbo([&]() noexcept { ++calls; ex.release(); }));
        ex->dispatch(task_wrapper_sbo([&, keep = token]() noexcept { ++calls; }));
        token.reset();
    };
    if (nested) {
        ex->dispatch(task_wrapper_sbo([&]() noexcept {
            enqueue_release();
            g_main_context_iteration(context, TRUE);
        }));
    } else enqueue_release();
    g_main_context_iteration(context, TRUE);
    ok = ok && !ex && calls == 1 && observer.expired();
    g_main_context_unref(context);
    return ok;
}

template <size_t Capacity>
bool concurrent_waves() {
    auto* context = g_main_context_new();
    bool ok = true;
    {
        gsource_executor<Capacity> ex;
        ok = ex.register_to(context) == 0;
        constexpr int producers = 4, waves = 100, per_wave = 31;
        std::atomic<int> delivered{0};
        int expected[producers]{};
        std::vector<std::thread> workers;
        for (int p = 0; p < producers; ++p) workers.emplace_back([&, p] {
            for (int wave = 0; wave < waves; ++wave) {
                for (int i = 0; i < per_wave; ++i) {
                    const int value = wave * per_wave + i;
                    ex.dispatch(task_wrapper_sbo([&, p, value]() noexcept {
                        ok = (expected[p]++ == value) && ok;
                        delivered.fetch_add(1, std::memory_order_release);
                    }));
                }
                // Each wave drains completely, exercising both idle wakeup
                // and producers racing the end of the previous callback.
                while (delivered.load(std::memory_order_acquire) < (wave + 1) * per_wave * producers)
                    std::this_thread::yield();
            }
        });
        while (delivered.load(std::memory_order_acquire) != producers * waves * per_wave)
            g_main_context_iteration(context, TRUE);
        for (auto& worker : workers) worker.join();
        for (int count : expected) ok = (count == waves * per_wave) && ok;
    }
    g_main_context_unref(context);
    return ok;
}

int main() {
    const bool ok = exercise<1>() && exercise<2>() && destruction(false) && destruction(true) &&
        concurrent_waves<1>() && concurrent_waves<2>() && concurrent_waves<64>();
    std::printf("[%s] GSource full queue, nested dispatch/destruction, MPSC waves and wakeup rearming\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
