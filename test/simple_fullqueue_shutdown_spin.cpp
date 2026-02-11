#include <thread>
#include <atomic>
#include <chrono>
#include <cstdio>
#include "executor/simple_executor.h"
int main(){
    flux_foundry::simple_executor<1> ex;
    std::atomic<int> done{0};
    ex.dispatch(flux_foundry::task_wrapper_sbo([&]() noexcept {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        done.fetch_add(1, std::memory_order_relaxed);
    }));

    std::thread p([&](){
        ex.dispatch(flux_foundry::task_wrapper_sbo([&]() noexcept {
            done.fetch_add(1, std::memory_order_relaxed);
        }));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ex.shutdown();
    p.join();
    std::printf("done=%d\n", done.load(std::memory_order_relaxed));
    return 0;
}
