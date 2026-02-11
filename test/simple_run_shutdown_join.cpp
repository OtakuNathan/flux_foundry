#include <thread>
#include <chrono>
#include <cstdio>
#include "executor/simple_executor.h"
int main(){
    flux_foundry::simple_executor<8> ex;
    std::thread t([&](){ ex.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ex.shutdown();
    t.join();
    std::puts("joined");
    return 0;
}
