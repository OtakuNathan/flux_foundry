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
// Owner-only experiment. Activity is sampled after uv_run returns; no other
// thread reads these metrics and no executor queue/close protocol is changed.
void run_hybrid(uv_loop_t* loop, load_probe::Ns idle_ns) {
    using namespace load_probe;
    static_assert(Clock::is_steady, "hybrid idle deadlines require a monotonic clock");
    static_assert(std::numeric_limits<Ns>::is_signed && std::numeric_limits<Ns>::digits >= 63,
                  "hybrid idle durations require signed 64-bit nanoseconds");
#if UV_VERSION_HEX >= 0x012d00
    uv_metrics_t metrics{};
    require(uv_metrics_info(loop, &metrics) == 0, "uv_metrics_info failed");
    auto events = metrics.events;
    auto last_activity = now_ns();
    auto mode = UV_RUN_NOWAIT;
    std::uint64_t polls = 0, waits = 0;
    for (;;) {
        if (mode == UV_RUN_NOWAIT) ++polls; else ++waits;
        if (!uv_run(loop, mode)) break;
        require(uv_metrics_info(loop, &metrics) == 0, "uv_metrics_info failed");
        const auto now = now_ns();
        if (metrics.events != events) last_activity = now;
        events = metrics.events;
        mode = now - last_activity < idle_ns ? UV_RUN_NOWAIT : UV_RUN_ONCE;
        if (mode == UV_RUN_NOWAIT) relax();
    }
    // Whole-process diagnostics, including startup, warmup and teardown.
    std::fprintf(stderr, "hybrid_process_turns poll=%llu wait=%llu idle_ns=%lld\n",
        static_cast<unsigned long long>(polls), static_cast<unsigned long long>(waits),
        static_cast<long long>(idle_ns));
#else
    (void)loop; (void)idle_ns;
    require(false, "hybrid loop requires libuv >= 1.45.0");
#endif
}
}

int main(int argc, char** argv) {
    using namespace load_probe;
    if (argc < 8 || argc > 10) {
        std::fprintf(stderr, "usage: uv_handoff raw|ordinary-fast|normal PROFILE SAMPLES WORK BUDGET OWNER_CPU PRODUCER_CPU [adaptive|spin [blocking|poll|hybrid-NANOSECONDS]]\n");
        return 2;
    }
    const char* mode = argv[1];
    const bool compare = argc >= 9;
    const bool loop_compare = argc == 10;
    const bool poll = loop_compare && !std::strcmp(argv[9], "poll");
    const bool hybrid = loop_compare && !std::strncmp(argv[9], "hybrid-", 7);
    const auto idle_ns = hybrid ? number(argv[9] + 7) : 0;
    require(!hybrid || (idle_ns > 0 && idle_ns <= 1000000), "hybrid idle window must be 1..1000000 ns");
    require(!loop_compare || poll || hybrid || !std::strcmp(argv[9], "blocking"), "invalid loop policy");
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
    unsigned long long consumer_tid = 0;
    std::atomic<bool> ready{false}, close_returned{false};
    std::thread owner([&] {
        ff_bench::name_trace_thread("ff-uv-consumer");
        consumer_tid = ff_bench::trace_thread_id();
        pin(o.consumer_cpu);
        uv_loop_t loop{};
        require(uv_loop_init(&loop) == 0, "uv_loop_init failed");
        {
            uv_executor ex(&loop, o.capacity, budget);
            executor = &ex;
            ready.store(true, std::memory_order_release);
            if (hybrid) {
                run_hybrid(&loop, static_cast<Ns>(idle_ns));
            } else if (poll) {
                // Keep the normal async callback, queue and wake protocol;
                // change only how the owner drives its borrowed event loop.
                while (uv_run(&loop, UV_RUN_NOWAIT)) relax();
            } else {
                uv_run(&loop, UV_RUN_DEFAULT);
            }
            require(ex.closed(), "executor did not close");
            // Public object lifetime includes the producer's close() call.
            while (!close_returned.load(std::memory_order_acquire)) relax();
        }
        require(uv_loop_close(&loop) == 0, "uv_loop_close failed");
    });
    while (!ready.load(std::memory_order_acquire)) relax();
    // The ready release also publishes the diagnostic consumer TID.
    ff_bench::await_trace_attachment(consumer_tid);
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
    print_csv_header("phase,budget,", loop_compare
        ? ",backoff,retries,scheduler_yields,process_cpu_ms,cpu_percent,loop_policy"
        : compare ? ",backoff,retries,scheduler_yields,process_cpu_ms,cpu_percent" : "");
    Options warmup = o;
    warmup.samples = std::min<std::size_t>(o.samples, 4096);
    const auto warm_prefix = std::string("warmup,") + std::to_string(budget) + ",";
    const auto warm_time = measured_run(port, o.capacity, warmup, profiles[1], mode, 0,
        warm_prefix.c_str(), !compare, nullptr, compare ? &process_monitor : nullptr);
    auto print_retry_stats = [&](const ProbeTiming& time) {
        require(time.wall_ns > 0 && time.process_cpu_ns >= 0, "invalid process CPU measurement");
        if (spin) require(stats.scheduler_yields == 0, "spin threshold exhausted");
        std::printf("%s,%llu,%llu,%.3f,%.3f", argv[8],
            static_cast<unsigned long long>(stats.retries),
            static_cast<unsigned long long>(stats.scheduler_yields),
            time.process_cpu_ns / 1e6, 100.0 * time.process_cpu_ns / time.wall_ns);
        if (loop_compare) std::printf(",%s", argv[9]);
        std::printf("\n");
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
