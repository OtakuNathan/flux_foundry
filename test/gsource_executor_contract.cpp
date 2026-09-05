#include "executor/gsource_executor.h"
#include <cstdio>
#include <thread>

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

int main() {
    const bool ok = exercise<1>() && exercise<2>();
    std::printf("[%s] GSource full queue, nested dispatch and producer wakeup\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
