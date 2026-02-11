#include <thread>
#include <atomic>
#include <chrono>
#include <cstdio>
#include "executor/simple_executor.h"

int main(){
    flux_foundry::simple_executor<1> ex;
    std::atomic<int> done{0};

    ex.dispatch(flux_foundry::task_wrapper_sbo([&]() noexcept {
        done.fetch_add(1, std::memory_order_relaxed);
    }));

    ex.shutdown();

    std::thread worker([&](){ ex.run(); });
    worker.join();

    std::printf("done=%d\n", done.load(std::memory_order_relaxed));
    return done.load(std::memory_order_relaxed) == 1 ? 0 : 5;
}
