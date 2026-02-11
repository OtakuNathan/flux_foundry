#include <thread>
#include <chrono>
#include "executor/simple_executor.h"
int main(){
    flux_foundry::simple_executor<8> ex;
    std::thread t([&](){ ex.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ex.shutdown();
    t.join();
    ex.dispatch(flux_foundry::task_wrapper_sbo([]() noexcept {}));
    return 0;
}
