#include "executor/simple_executor.h"
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
using namespace flux_foundry;

template <size_t Capacity>
bool exercise() {
    int delivered = 0;
    {
        simple_executor<Capacity> ex;
        ex.dispatch(task_wrapper_sbo([&]() noexcept {
            for (size_t i = 0; i < Capacity + 3; ++i) {
                ex.dispatch(task_wrapper_sbo([&]() noexcept { ++delivered; }));
            }
            ex.try_shutdown();
        }));
        ex.run();
        if (delivered != static_cast<int>(Capacity + 3)) return false;
    }
    delivered = 0;
    {
        simple_executor<Capacity> ex;
        std::mutex mutex;
        std::condition_variable cv;
        bool entered = false, release = false;
        ex.dispatch(task_wrapper_sbo([&]() noexcept {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true;
            cv.notify_one();
            cv.wait(lock, [&] { return release; });
            ++delivered;
        }));
        std::thread consumer([&] { ex.run(); });
        { std::unique_lock<std::mutex> lock(mutex); cv.wait(lock, [&] { return entered; }); }
        // All these tasks are admitted before shutdown, with the consumer held.
        for (size_t i = 0; i < Capacity; ++i) {
            ex.dispatch(task_wrapper_sbo([&]() noexcept { ++delivered; }));
        }
        const bool shutdown = ex.try_shutdown() && ex.try_shutdown();
        { std::lock_guard<std::mutex> lock(mutex); release = true; }
        cv.notify_one();
        consumer.join();
        if (!shutdown || delivered != static_cast<int>(Capacity + 1)) return false;
    }
    return true;
}
int main() {
    const bool ok = exercise<1>() && exercise<2>();
    std::printf("[%s] simple executor nested dispatch and shutdown drain\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
