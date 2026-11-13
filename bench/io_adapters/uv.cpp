// Same source is compiled against the current and baseline uv_executor headers.
#include "executor/uv_executor.h"
#include "common.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using namespace flux_foundry;
using probe::require;

struct metrics { double ns_per_op, close_us; std::uint64_t retries; };

metrics measure(unsigned producers, std::size_t count, std::size_t capacity,
                std::size_t budget, unsigned owner_cpu, unsigned first_producer_cpu) {
    probe::pin(owner_cpu);
    uv_loop_t loop;
    require(uv_loop_init(&loop) == 0, "uv_loop_init failed");
    metrics result{};
    {
        uv_executor executor(&loop, capacity, budget);
        std::vector<std::size_t> sequence(std::max(1u, producers), 0);
        std::vector<std::uint64_t> retries(producers, 0);
        std::vector<std::thread> threads;
        std::atomic<unsigned> ready{0};
        std::atomic<bool> go{false};
        std::size_t completed = 0;
        probe::clock::time_point finished;
        auto execute = [&](unsigned p, std::size_t i) noexcept {
            require(sequence[p]++ == i, "task duplicated, lost or reordered within producer");
            if (++completed == count) {
                finished = probe::clock::now();
                executor.close();
            }
        };
        for (unsigned p = 0; p < producers; ++p) {
            threads.emplace_back([&, p] {
                probe::pin(first_producer_cpu + p);
                ready.fetch_add(1, std::memory_order_release);
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                const auto n = count / producers + (p < count % producers);
                std::uint64_t rejected = 0;
                for (std::size_t i = 0; i < n; ++i) {
                    task_wrapper_sbo task([&, p, i]() noexcept { execute(p, i); });
                    while (!executor.try_dispatch(std::move(task))) {
                        require(bool(task), "rejection consumed task");
                        ++rejected;
                        std::this_thread::yield();
                    }
                }
                retries[p] = rejected;
            });
        }
        while (ready.load(std::memory_order_acquire) != producers) std::this_thread::yield();
        const auto start = probe::clock::now();
        go.store(true, std::memory_order_release);
        if (!producers) {
            for (std::size_t i = 0; i < count; ++i)
                executor.dispatch(task_wrapper_sbo([&, i]() noexcept { execute(0, i); }));
        }
        uv_run(&loop, UV_RUN_DEFAULT);
        const auto closed = probe::clock::now();
        for (auto& thread : threads) thread.join();
        require(completed == count && executor.closed(), "incomplete drain/close");
        result.ns_per_op = probe::ns(start, finished) / count;
        result.close_us = probe::ns(finished, closed) / 1000;
        for (auto n : retries) result.retries += n;
    }
    require(uv_loop_close(&loop) == 0, "uv_loop_close failed");
    return result;
}

int main(int argc, char** argv) {
    if (argc != 8) {
        std::fprintf(stderr, "usage: uv LABEL PRODUCERS COUNT CAPACITY BUDGET OWNER_CPU FIRST_PRODUCER_CPU\n");
        return 2;
    }
    const unsigned producers = std::stoul(argv[2]);
    const auto count = std::stoull(argv[3]), capacity = std::stoull(argv[4]), budget = std::stoull(argv[5]);
    const unsigned owner = std::stoul(argv[6]), first = std::stoul(argv[7]);
    require(count && capacity && budget, "count/capacity/budget must be positive");
    // Match a multithreaded host even for owner-inline: glibc/libstdc++ may
    // otherwise use single-thread shortcuts for mutexes and shared_ptr counts.
    std::thread([] {}).join();
    measure(producers, std::min<std::size_t>(count, 16384), capacity, budget, owner, first);
    const auto m = measure(producers, count, capacity, budget, owner, first);
    std::printf("uv,%s,%u,%llu,%llu,%llu,%.3f,%.3f,%llu\n", argv[1], producers,
        count, capacity, budget, m.ns_per_op, m.close_us,
        static_cast<unsigned long long>(m.retries));
}
