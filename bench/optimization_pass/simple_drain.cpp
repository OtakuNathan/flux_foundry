// Prefilled-queue drain only. This isolates successful-pop work; it does not
// measure cross-core contention, arrival latency, or general executor throughput.
#include "executor/simple_executor.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace flux_foundry;

int main(int argc, char** argv) {
    if (argc > 2) return 2;
    const auto rounds = argc == 2 ? std::strtoull(argv[1], nullptr, 10) : 1000;
    if (!rounds || rounds > 1000000) return 2;
    std::thread([] {}).join();
    constexpr std::uint64_t n = 1024;
    constexpr unsigned warmup = 100;
    double total_ns = 0;
    std::uint64_t total_sum = 0;
    for (std::uint64_t r = 0; r != warmup + rounds; ++r) {
        simple_executor<n> ex;
        std::uint64_t sum = 0;
        for (std::uint64_t i = 1; i <= n; ++i)
            ex.dispatch(task_wrapper_sbo([&sum, i]() noexcept { sum += i; }));
        if (!ex.try_shutdown()) std::abort();
        const auto start = std::chrono::steady_clock::now();
        ex.run();
        const auto finish = std::chrono::steady_clock::now();
        if (sum != n * (n + 1) / 2) std::abort();
        if (r >= warmup) total_ns += std::chrono::duration<double, std::nano>(finish - start).count();
        total_sum += sum;
    }
    std::printf("rounds,tasks_per_round,ns_per_task,checksum\n%llu,%llu,%.6f,%llu\n",
        static_cast<unsigned long long>(rounds), static_cast<unsigned long long>(n),
        total_ns / (n * rounds), static_cast<unsigned long long>(total_sum));
}
