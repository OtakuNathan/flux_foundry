#include <thread>
#include <atomic>
#include <chrono>
#include <cstdio>
#include "executor/simple_executor.h"

int main(){
    flux_foundry::simple_executor<1> ex;
    std::atomic<int> done{0};

    ex.dispatch(flux_foundry::task_wrapper_sbo([&]() noexcept {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        done.fetch_add(1, std::memory_order_relaxed);
    }));

    std::thread producer([&](){
        ex.dispatch(flux_foundry::task_wrapper_sbo([&]() noexcept {
            done.fetch_add(1, std::memory_order_relaxed);
        }));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::thread worker([&](){ ex.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ex.shutdown();

    producer.join();
    worker.join();

    const int value = done.load(std::memory_order_relaxed);
    std::printf("done=%d\n", value);
    return value == 2 ? 0 : 3;
}
