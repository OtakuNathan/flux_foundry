// Matched UV submission controls: both lanes construct the same task groups.
// This is saturated enqueue/drain throughput, not request latency or I/O fairness.
#include "executor/uv_executor.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace {
using clock_type = std::chrono::steady_clock;
using executor_type = flux_foundry::uv_executor;
void require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::abort(); }
}
void pin(int cpu) {
#if defined(__linux__)
    if (cpu < 0) return;
    require(cpu < CPU_SETSIZE, "CPU index exceeds CPU_SETSIZE");
    cpu_set_t affinity;
    CPU_ZERO(&affinity); CPU_SET(cpu, &affinity);
    require(pthread_setaffinity_np(pthread_self(), sizeof(affinity), &affinity) == 0,
            "requested CPU affinity is unavailable");
#else
    require(cpu < 0, "CPU pinning is supported only on Linux");
#endif
}
struct measurement { double ns_per_task; std::uint64_t retries; };
measurement run(bool batch_mode, executor_type::wake_fence_policy fence,
                std::size_t per_producer, std::size_t width, std::size_t producer_count,
                std::size_t capacity, std::size_t budget, int owner_cpu, int first_producer_cpu) {
    std::atomic<bool> ready{false}, start{false}, complete{false}, close_returned{false};
    std::atomic<std::size_t> producers_ready{0}, producers_finished{0};
    executor_type* executor = nullptr;
    const auto total = per_producer * producer_count;
    std::size_t completed = 0;
    std::vector<std::size_t> expected(producer_count, 0);
    std::vector<std::uint64_t> retries(producer_count, 0);
    std::thread owner([&] {
        pin(owner_cpu);
        uv_loop_t loop{}; require(uv_loop_init(&loop) == 0, "uv_loop_init failed");
        {
            executor_type local(&loop, capacity, budget, fence);
            executor = &local;
            ready.store(true, std::memory_order_release);
            require(uv_run(&loop, UV_RUN_DEFAULT) == 0 && local.closed(), "UV close did not drain");
            // Keep the public object alive until the producer's close returns.
            while (!close_returned.load(std::memory_order_acquire)) std::this_thread::yield();
        }
        require(uv_loop_close(&loop) == 0, "UV handle leak");
    });
    while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
    std::vector<std::thread> producers;
    for (std::size_t p = 0; p < producer_count; ++p) producers.emplace_back([&, p] {
        pin(first_producer_cpu < 0 ? -1 : first_producer_cpu + static_cast<int>(p));
        std::vector<executor_type::task_t> tasks(width);
        producers_ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (std::size_t base = 0; base < per_producer; base += width) {
            const auto n = std::min(width, per_producer - base);
            for (std::size_t i = 0; i < n; ++i) {
                const auto sequence = base + i;
                tasks[i] = executor_type::task_t([&, p, sequence]() noexcept {
                    require(expected[p]++ == sequence, "duplicate, dropped or out-of-order task");
                    if (++completed == total) complete.store(true, std::memory_order_release);
                });
            }
            if (batch_mode) {
                std::size_t accepted = 0;
                while (accepted != n) {
                    const auto progress = executor->try_dispatch_batch(tasks.data() + accepted, n - accepted);
                    accepted += progress;
                    if (accepted != n) { ++retries[p]; std::this_thread::yield(); }
                }
            } else {
                for (std::size_t i = 0; i < n; ++i) {
                    while (!executor->try_dispatch(std::move(tasks[i]))) {
                        require(bool(tasks[i]), "rejected task was consumed");
                        ++retries[p]; std::this_thread::yield();
                    }
                }
            }
        }
        producers_finished.fetch_add(1, std::memory_order_release);
    });
    while (producers_ready.load(std::memory_order_acquire) != producer_count) std::this_thread::yield();
    const auto begin = clock_type::now();
    start.store(true, std::memory_order_release);
    while (!complete.load(std::memory_order_acquire)
            || producers_finished.load(std::memory_order_acquire) != producer_count)
        std::this_thread::yield();
    const auto elapsed = std::chrono::duration<double, std::nano>(clock_type::now() - begin).count();
    for (auto& producer : producers) producer.join();
    executor->close();
    close_returned.store(true, std::memory_order_release);
    owner.join();
    std::uint64_t total_retries = 0;
    for (std::size_t p = 0; p < producer_count; ++p) {
        require(expected[p] == per_producer, "incorrect final completion count");
        total_retries += retries[p];
    }
    return {elapsed / total, total_retries};
}
}

int main(int argc, char** argv) {
    if (argc != 8 && argc != 10) {
        std::fprintf(stderr, "usage: uv_batch single|batch REQUESTS_PER_PRODUCER BATCH PRODUCERS CAPACITY BUDGET automatic|fence [OWNER_CPU FIRST_PRODUCER_CPU]\n");
        return 2;
    }
    require(!std::strcmp(argv[1], "single") || !std::strcmp(argv[1], "batch"), "invalid submission mode");
    require(!std::strcmp(argv[7], "automatic") || !std::strcmp(argv[7], "fence"), "invalid fence mode");
    const auto count = static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10));
    const auto width = static_cast<std::size_t>(std::strtoull(argv[3], nullptr, 10));
    const auto producers = static_cast<std::size_t>(std::strtoull(argv[4], nullptr, 10));
    const auto capacity = static_cast<std::size_t>(std::strtoull(argv[5], nullptr, 10));
    const auto budget = static_cast<std::size_t>(std::strtoull(argv[6], nullptr, 10));
    require(count && count <= 1000000000 && width && width <= 65536 && producers && producers <= 64
            && capacity && budget, "invalid workload dimensions");
    const int owner_cpu = argc == 10 ? std::atoi(argv[8]) : -1;
    const int producer_cpu = argc == 10 ? std::atoi(argv[9]) : -1;
    const bool batch = !std::strcmp(argv[1], "batch");
    const auto fence = !std::strcmp(argv[7], "fence") ? executor_type::wake_fence_policy::always_fence
                                                    : executor_type::wake_fence_policy::runtime_compatible;
    run(batch, fence, std::min<std::size_t>(count, 8192), width, producers, capacity, budget, owner_cpu, producer_cpu);
    const auto result = run(batch, fence, count, width, producers, capacity, budget, owner_cpu, producer_cpu);
    std::printf("mode,fence,requests_per_producer,batch,producers,capacity,budget,ns_per_task,retries,libuv\n");
    std::printf("%s,%s,%zu,%zu,%zu,%zu,%zu,%.3f,%llu,%s\n", argv[1], argv[7], count, width,
                producers, capacity, budget, result.ns_per_task,
                static_cast<unsigned long long>(result.retries), uv_version_string());
}
