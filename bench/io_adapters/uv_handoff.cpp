// Scheduled one-producer handoff through the production uv_executor.
// Shared measurement helpers include entry points used by other probes.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "bench/load_probe.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#include "executor/uv_executor.h"
#include <string>
#include <ctime>
#include <limits>

namespace {
constexpr std::size_t adaptive_limit = 16;
constexpr std::size_t spin_limit = (std::numeric_limits<std::size_t>::max)();
static_assert(std::is_same<flux_foundry::backoff_strategy<>,
              flux_foundry::backoff_strategy<adaptive_limit, 1024>>::value,
              "update the benchmark if production backoff defaults change");
static_assert(std::numeric_limits<std::size_t>::digits >= 64,
              "spin comparison requires a 64-bit step counter");
struct RetryStats {
    flux_foundry::uv_executor* executor;
    std::uint64_t retries = 0, scheduler_yields = 0;
};
template<std::size_t Limit>
void retry_dispatch(void* ptr, flux_foundry::task_wrapper_sbo&& task) noexcept {
    auto& stats = *static_cast<RetryStats*>(ptr);
    // Reuse the production exponential backoff. Only the scheduler-yield
    // threshold changes; max_loop remains the production default (1024).
    for (flux_foundry::backoff_strategy<Limit> backoff;
         !stats.executor->try_dispatch(std::move(task)); backoff.yield()) {
        load_probe::require(bool(task) && !stats.executor->closed(), "unexpected dispatch rejection");
        ++stats.retries;
        if (backoff.steps >= Limit) ++stats.scheduler_yields;
    }
}
load_probe::Ns process_cpu(void*) {
    const auto ticks = std::clock();
    load_probe::require(ticks != std::clock_t(-1), "process CPU clock unavailable");
    return static_cast<load_probe::Ns>(static_cast<long double>(ticks) * 1000000000 / CLOCKS_PER_SEC);
}
}

int main(int argc, char** argv) {
    using namespace load_probe;
    if (argc != 8 && argc != 9) {
        std::fprintf(stderr, "usage: uv_handoff raw|ordinary-fast|normal PROFILE SAMPLES WORK BUDGET OWNER_CPU PRODUCER_CPU [adaptive|spin]\n");
        return 2;
    }
    const char* mode = argv[1];
    const bool compare = argc == 9;
    const bool spin = compare && !std::strcmp(argv[8], "spin");
    require(!compare || spin || !std::strcmp(argv[8], "adaptive"), "invalid backoff policy");
    require(!std::strcmp(mode, "raw") || !std::strcmp(mode, "ordinary-fast") ||
            !std::strcmp(mode, "normal"), "invalid mode");
    const Profile* profile = nullptr;
    for (const auto& p : profiles) if (!std::strcmp(p.name, argv[2])) profile = &p;
    require(profile != nullptr, "invalid arrival profile");
    Options o;
    o.samples = number(argv[3]);
    o.work = number(argv[4]);
    const auto budget = number(argv[5]);
    o.consumer_cpu = !std::strcmp(argv[6], "-1") ? -1 : static_cast<int>(number(argv[6]));
    o.producer_cpu = !std::strcmp(argv[7], "-1") ? -1 : static_cast<int>(number(argv[7]));
    o.capacity = 1024;
    require(o.samples > 0 && budget > 0 &&
            (o.producer_cpu < 0 || o.consumer_cpu < 0 || o.producer_cpu != o.consumer_cpu),
            "positive samples/budget and distinct CPUs required");
    pin(o.producer_cpu);
    ff_bench::name_trace_thread("ff-producer");

    uv_executor* executor = nullptr;
    std::atomic<bool> ready{false}, close_returned{false};
    std::thread owner([&] {
        ff_bench::name_trace_thread("ff-uv-consumer");
        pin(o.consumer_cpu);
        uv_loop_t loop{};
        require(uv_loop_init(&loop) == 0, "uv_loop_init failed");
        {
            uv_executor ex(&loop, o.capacity, budget);
            executor = &ex;
            ready.store(true, std::memory_order_release);
            uv_run(&loop, UV_RUN_DEFAULT);
            require(ex.closed(), "executor did not close");
            // Public object lifetime includes the producer's close() call.
            while (!close_returned.load(std::memory_order_acquire)) relax();
        }
        require(uv_loop_close(&loop) == 0, "uv_loop_close failed");
    });
    while (!ready.load(std::memory_order_acquire)) relax();
    Port port{executor, [](void* ptr, task_wrapper_sbo&& task) noexcept {
        auto* ex = static_cast<uv_executor*>(ptr);
        // No-drop benchmark policy: wait for queue space. Production dispatch
        // itself rejects full queues; this wrapper makes that policy explicit.
        for (backoff_strategy<> backoff; !ex->try_dispatch(std::move(task)); backoff.yield()) {
            require(bool(task) && !ex->closed(), "unexpected dispatch rejection");
        }
    }};
    RetryStats stats{executor};
    if (compare) {
        port = {&stats, spin ? retry_dispatch<spin_limit> : retry_dispatch<adaptive_limit>};
    }
    const CpuMonitor process_monitor{nullptr, process_cpu};
    print_csv_header("phase,budget,", compare ? ",backoff,retries,scheduler_yields,process_cpu_ms,cpu_percent" : "");
    Options warmup = o;
    warmup.samples = std::min<std::size_t>(o.samples, 4096);
    const auto warm_prefix = std::string("warmup,") + std::to_string(budget) + ",";
    const auto warm_time = measured_run(port, o.capacity, warmup, profiles[1], mode, 0,
        warm_prefix.c_str(), !compare, nullptr, compare ? &process_monitor : nullptr);
    auto print_retry_stats = [&](const ProbeTiming& time) {
        require(time.wall_ns > 0 && time.process_cpu_ns >= 0, "invalid process CPU measurement");
        if (spin) require(stats.scheduler_yields == 0, "spin threshold exhausted");
        std::printf("%s,%llu,%llu,%.3f,%.3f\n", argv[8],
            static_cast<unsigned long long>(stats.retries),
            static_cast<unsigned long long>(stats.scheduler_yields),
            time.process_cpu_ns / 1e6, 100.0 * time.process_cpu_ns / time.wall_ns);
    };
    if (compare) print_retry_stats(warm_time);
    stats.retries = stats.scheduler_yields = 0;
    const auto prefix = std::string("measured,") + std::to_string(budget) + ",";
    const auto time = measured_run(port, o.capacity, o, *profile, mode, 1,
        prefix.c_str(), !compare, nullptr, compare ? &process_monitor : nullptr);
    if (compare) print_retry_stats(time);
    executor->close();
    close_returned.store(true, std::memory_order_release);
    owner.join();
}
