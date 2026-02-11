#include <atomic>
#include <thread>
#include <chrono>
#include <cstdio>
#include "executor/simple_executor.h"

int main(){
    flux_foundry::simple_executor<1> ex;
    std::atomic<int> c1{0}, c2{0};

    ex.dispatch(flux_foundry::task_wrapper_sbo([&]() noexcept { c1.fetch_add(1, std::memory_order_relaxed); }));

    std::thread producer([&](){
        ex.dispatch(flux_foundry::task_wrapper_sbo([&]() noexcept { c2.fetch_add(1, std::memory_order_relaxed); }));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ex.shutdown();

    producer.join();
    std::printf("c1=%d c2=%d\n", c1.load(), c2.load());
    return 0;
}
