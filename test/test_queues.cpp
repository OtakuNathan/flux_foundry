#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <thread>
#include <vector>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif
#include "utility/concurrent_queues.h"

using namespace flux_foundry;
using namespace std::chrono;

// 测试配置
constexpr size_t CAPACITY = (1u << 16);
constexpr int DEFAULT_DURATION_SEC = 5;
constexpr size_t LATENCY_SAMPLE_STRIDE = 4096;
constexpr uint64_t ID_SAMPLE_MASK = (1u << 13) - 1;
static_assert((LATENCY_SAMPLE_STRIDE & (LATENCY_SAMPLE_STRIDE - 1)) == 0,
              "latency sample stride must be power-of-two");

struct alignas(OPTIMIZED_ALIGN) counter_slot {
    uint64_t value = 0;
};

template <typename Slots>
uint64_t sum_slots(const Slots& slots) noexcept {
    uint64_t total = 0;
    for (const auto& slot : slots) {
        total += slot.value;
    }
    return total;
}

using latency_samples = std::vector<uint32_t>;
using id_samples = std::vector<uint64_t>;

struct latency_summary {
    size_t samples = 0;
    uint32_t p50_ns = 0;
    uint32_t p99_ns = 0;
    uint32_t max_ns = 0;
};

bool should_sample(uint64_t ticket) noexcept {
    return (ticket & (LATENCY_SAMPLE_STRIDE - 1)) == 0;
}

uint32_t elapsed_ns(steady_clock::time_point start, steady_clock::time_point stop) noexcept {
    const auto ns = duration_cast<nanoseconds>(stop - start).count();
    if (ns <= 0) {
        return 0;
    }
    if (ns >= static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
        return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(ns);
}

latency_summary summarize_latency(latency_samples samples) {
    latency_summary summary{};
    summary.samples = samples.size();
    if (samples.empty()) {
        return summary;
    }

    std::sort(samples.begin(), samples.end());
    const size_t p50_index = samples.size() / 2;
    const size_t p99_index = (samples.size() * 99) / 100;
    summary.p50_ns = samples[p50_index];
    summary.p99_ns = samples[std::min(p99_index, samples.size() - 1)];
    summary.max_ns = samples.back();
    return summary;
}

template <typename SamplesByThread>
latency_samples merge_samples(const SamplesByThread& samples_by_thread) {
    size_t total = 0;
    for (const auto& samples : samples_by_thread) {
        total += samples.size();
    }

    latency_samples merged;
    merged.reserve(total);
    for (const auto& samples : samples_by_thread) {
        merged.insert(merged.end(), samples.begin(), samples.end());
    }
    return merged;
}

template <typename SamplesByThread>
id_samples merge_id_samples(const SamplesByThread& samples_by_thread) {
    size_t total = 0;
    for (const auto& samples : samples_by_thread) {
        total += samples.size();
    }

    id_samples merged;
    merged.reserve(total);
    for (const auto& samples : samples_by_thread) {
        merged.insert(merged.end(), samples.begin(), samples.end());
    }
    return merged;
}

bool has_duplicate_sample(id_samples samples) {
    if (samples.empty()) {
        return false;
    }

    std::sort(samples.begin(), samples.end());
    for (size_t i = 1; i < samples.size(); ++i) {
        if (samples[i] == samples[i - 1]) {
            return true;
        }
    }
    return false;
}

void print_latency(const char* label, const latency_summary& summary) {
    if (summary.samples == 0) {
        std::cout << ' ' << label << "_samples=0";
        return;
    }

    std::cout << ' ' << label << "_samples=" << summary.samples
              << ' ' << label << "_p50_ns=" << summary.p50_ns
              << ' ' << label << "_p99_ns=" << summary.p99_ns
              << ' ' << label << "_max_ns=" << summary.max_ns;
}

bool bind_current_thread(size_t logical_slot) noexcept {
#if defined(__APPLE__)
    const integer_t tag = static_cast<integer_t>(logical_slot + 1);
    thread_affinity_policy_data_t policy{tag};
    return thread_policy_set(mach_thread_self(),
                             THREAD_AFFINITY_POLICY,
                             reinterpret_cast<thread_policy_t>(&policy),
                             THREAD_AFFINITY_POLICY_COUNT) == KERN_SUCCESS;
#elif defined(__linux__)
    const unsigned cpu_count = std::max(1u, std::thread::hardware_concurrency());
    const size_t cpu_index = logical_slot % cpu_count;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(static_cast<int>(cpu_index), &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) == 0;
#else
    (void)logical_slot;
    return false;
#endif
}

void print_result(const char* name, uint64_t pushed, uint64_t popped_during_run,
                  uint64_t drained_after_stop, milliseconds elapsed,
                  const latency_summary& push_latency,
                  const latency_summary& pop_latency,
                  size_t sampled_ids,
                  bool duplicate_sample) {
    const uint64_t popped_total = popped_during_run + drained_after_stop;
    const double seconds = static_cast<double>(elapsed.count()) / 1000.0;
    const double mops = seconds > 0.0 ? static_cast<double>(popped_total) / seconds / 1000000.0 : 0.0;

    std::cout << name
              << " push=" << pushed
              << " pop=" << popped_during_run
              << " drain=" << drained_after_stop
              << " elapsed_ms=" << elapsed.count()
              << " throughput=" << mops << " Mops/s";
    print_latency("push", push_latency);
    print_latency("pop", pop_latency);
    std::cout << " sampled_ids=" << sampled_ids
              << " duplicate_sample=" << (duplicate_sample ? "yes" : "no")
              << '\n';
}

void require_balanced(const char* name, uint64_t pushed, uint64_t popped_during_run,
                      uint64_t drained_after_stop) {
    const uint64_t popped_total = popped_during_run + drained_after_stop;
    if (pushed != popped_total) {
        std::cerr << name << " mismatch: push=" << pushed << " pop_total=" << popped_total << '\n';
        std::exit(1);
    }
}

void require_no_duplicates(const char* name, bool duplicate_sample) {
    if (duplicate_sample) {
        std::cerr << name << " duplicate sampled pop detected\n";
        std::exit(1);
    }
}

void wait_for_start(const std::atomic<bool>& start) noexcept {
    backoff_strategy<> backoff;
    while (!start.load(std::memory_order_acquire)) {
        backoff.yield();
    }
}

int parse_positive_or_default(const char* text, int fallback) noexcept {
    if (!text || *text == '\0') {
        return fallback;
    }

    int value = 0;
    for (const char* p = text; *p; ++p) {
        if (*p < '0' || *p > '9') {
            return fallback;
        }
        value = value * 10 + (*p - '0');
        if (value <= 0) {
            return fallback;
        }
    }
    return value > 0 ? value : fallback;
}

// SPSC 测试
void test_spsc(int duration_sec) {
    std::cout << "\n=== SPSC Queue (1P1C) ===\n";

    auto q = std::make_unique<spsc_queue<uint64_t, CAPACITY>>();
    std::atomic<bool> start{false}, stop{false};
    counter_slot push_cnt;
    counter_slot pop_cnt;
    latency_samples push_samples;
    latency_samples pop_samples;
    id_samples sampled_ids;

    std::thread producer([&] {
        bind_current_thread(0);
        wait_for_start(start);
        uint64_t id = 0;
        uint64_t local_push = 0;
        uint64_t sample_ticket = 0;
        latency_samples local_samples;
        local_samples.reserve(32768);
        while (!stop.load(std::memory_order_acquire)) {
            if (should_sample(sample_ticket++)) {
                const auto t_begin = steady_clock::now();
                if (q->try_emplace(uint64_t{id})) {
                    local_samples.push_back(elapsed_ns(t_begin, steady_clock::now()));
                    ++id;
                    ++local_push;
                }
            } else if (q->try_emplace(uint64_t{id})) {
                ++id;
                ++local_push;
            }
        }
        push_cnt.value = local_push;
        push_samples = std::move(local_samples);
    });

    std::thread consumer([&] {
        bind_current_thread(1);
        wait_for_start(start);
        uint64_t local_pop = 0;
        uint64_t sample_ticket = 0;
        latency_samples local_samples;
        local_samples.reserve(32768);
        id_samples local_ids;
        local_ids.reserve(8192);
        while (!stop.load(std::memory_order_acquire)) {
            if (should_sample(sample_ticket++)) {
                const auto t_begin = steady_clock::now();
                auto value = q->try_pop();
                if (value.has_value()) {
                    if ((value.get() & ID_SAMPLE_MASK) == 0) {
                        local_ids.push_back(value.get());
                    }
                    local_samples.push_back(elapsed_ns(t_begin, steady_clock::now()));
                    ++local_pop;
                }
            } else {
                auto value = q->try_pop();
                if (value.has_value()) {
                    if ((value.get() & ID_SAMPLE_MASK) == 0) {
                        local_ids.push_back(value.get());
                    }
                    ++local_pop;
                }
            }
        }
        pop_cnt.value = local_pop;
        pop_samples = std::move(local_samples);
        sampled_ids = std::move(local_ids);
    });

    auto t0 = steady_clock::now();
    start.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::seconds(duration_sec));
    stop.store(true, std::memory_order_release);
    producer.join();
    consumer.join();

    uint64_t drained = 0;
    while (true) {
        auto value = q->try_pop();
        if (!value.has_value()) {
            break;
        }
        if ((value.get() & ID_SAMPLE_MASK) == 0) {
            sampled_ids.push_back(value.get());
        }
        ++drained;
    }

    auto t1 = steady_clock::now();
    const bool duplicate_sample = has_duplicate_sample(sampled_ids);
    print_result("spsc", push_cnt.value, pop_cnt.value, drained, duration_cast<milliseconds>(t1 - t0),
                 summarize_latency(std::move(push_samples)),
                 summarize_latency(std::move(pop_samples)),
                 sampled_ids.size(),
                 duplicate_sample);
    require_balanced("spsc", push_cnt.value, pop_cnt.value, drained);
    require_no_duplicates("spsc", duplicate_sample);
}

// MPSC 测试
void test_mpsc(int producers, int duration_sec) {
    std::cout << "\n=== MPSC Queue (" << producers << "P1C) ===\n";

    auto q = std::make_unique<mpsc_queue<uint64_t, CAPACITY>>();
    std::atomic<bool> start{false}, stop{false};
    std::vector<counter_slot> push_cnt(producers);
    counter_slot pop_cnt;
    std::vector<latency_samples> push_samples(producers);
    latency_samples pop_samples;
    id_samples sampled_ids;

    std::vector<std::thread> threads;

    for (int i = 0; i < producers; ++i) {
        threads.emplace_back([&, i] {
            bind_current_thread(static_cast<size_t>(i));
            wait_for_start(start);
            uint64_t local_push = 0;
            uint64_t sample_ticket = 0;
            latency_samples local_samples;
            local_samples.reserve(16384);
            while (!stop.load(std::memory_order_acquire)) {
                const uint64_t id = local_push * static_cast<uint64_t>(producers) + static_cast<uint64_t>(i);
                if (should_sample(sample_ticket++)) {
                    const auto t_begin = steady_clock::now();
                    if (q->try_emplace(uint64_t{id})) {
                        local_samples.push_back(elapsed_ns(t_begin, steady_clock::now()));
                        ++local_push;
                    }
                } else if (q->try_emplace(uint64_t{id})) {
                    ++local_push;
                }
            }
            push_cnt[i].value = local_push;
            push_samples[i] = std::move(local_samples);
        });
    }

    threads.emplace_back([&] {
        bind_current_thread(static_cast<size_t>(producers));
        wait_for_start(start);
        uint64_t local_pop = 0;
        uint64_t sample_ticket = 0;
        latency_samples local_samples;
        local_samples.reserve(16384);
        id_samples local_ids;
        local_ids.reserve(8192);
        while (!stop.load(std::memory_order_acquire)) {
            if (should_sample(sample_ticket++)) {
                const auto t_begin = steady_clock::now();
                auto value = q->try_pop();
                if (value.has_value()) {
                    if ((value.get() & ID_SAMPLE_MASK) == 0) {
                        local_ids.push_back(value.get());
                    }
                    local_samples.push_back(elapsed_ns(t_begin, steady_clock::now()));
                    ++local_pop;
                }
            } else {
                auto value = q->try_pop();
                if (value.has_value()) {
                    if ((value.get() & ID_SAMPLE_MASK) == 0) {
                        local_ids.push_back(value.get());
                    }
                    ++local_pop;
                }
            }
        }
        pop_cnt.value = local_pop;
        pop_samples = std::move(local_samples);
        sampled_ids = std::move(local_ids);
    });

    auto t0 = steady_clock::now();
    start.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::seconds(duration_sec));
    stop.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    uint64_t drained = 0;
    while (true) {
        auto value = q->try_pop();
        if (!value.has_value()) {
            break;
        }
        if ((value.get() & ID_SAMPLE_MASK) == 0) {
            sampled_ids.push_back(value.get());
        }
        ++drained;
    }

    auto t1 = steady_clock::now();
    const bool duplicate_sample = has_duplicate_sample(sampled_ids);
    print_result("mpsc", sum_slots(push_cnt), pop_cnt.value, drained, duration_cast<milliseconds>(t1 - t0),
                 summarize_latency(merge_samples(push_samples)),
                 summarize_latency(std::move(pop_samples)),
                 sampled_ids.size(),
                 duplicate_sample);
    require_balanced("mpsc", sum_slots(push_cnt), pop_cnt.value, drained);
    require_no_duplicates("mpsc", duplicate_sample);
}

// MPMC 测试
void test_mpmc(int producers, int consumers, int duration_sec) {
    std::cout << "\n=== MPMC Queue (" << producers << "P" << consumers << "C) ===\n";

    auto q = std::make_unique<mpmc_queue<uint64_t, CAPACITY>>();
    std::atomic<bool> start{false}, stop{false};
    std::vector<counter_slot> push_cnt(producers);
    std::vector<counter_slot> pop_cnt(consumers);
    std::vector<latency_samples> push_samples(producers);
    std::vector<latency_samples> pop_samples(consumers);
    std::vector<id_samples> sampled_ids(consumers);

    std::vector<std::thread> threads;

    for (int i = 0; i < producers; ++i) {
        threads.emplace_back([&, i] {
            bind_current_thread(static_cast<size_t>(i));
            wait_for_start(start);
            uint64_t local_push = 0;
            uint64_t sample_ticket = 0;
            latency_samples local_samples;
            local_samples.reserve(16384);
            while (!stop.load(std::memory_order_acquire)) {
                const uint64_t id = local_push * static_cast<uint64_t>(producers) + static_cast<uint64_t>(i);
                if (should_sample(sample_ticket++)) {
                    const auto t_begin = steady_clock::now();
                    if (q->try_emplace(uint64_t{id})) {
                        local_samples.push_back(elapsed_ns(t_begin, steady_clock::now()));
                        ++local_push;
                    }
                } else if (q->try_emplace(uint64_t{id})) {
                    ++local_push;
                }
            }
            push_cnt[i].value = local_push;
            push_samples[i] = std::move(local_samples);
        });
    }

    for (int i = 0; i < consumers; ++i) {
        threads.emplace_back([&, i] {
            bind_current_thread(static_cast<size_t>(producers + i));
            wait_for_start(start);
            uint64_t local_pop = 0;
            uint64_t sample_ticket = 0;
            latency_samples local_samples;
            local_samples.reserve(16384);
            id_samples local_ids;
            local_ids.reserve(8192);
            while (!stop.load(std::memory_order_acquire)) {
                if (should_sample(sample_ticket++)) {
                    const auto t_begin = steady_clock::now();
                    auto value = q->try_pop();
                    if (value.has_value()) {
                        if ((value.get() & ID_SAMPLE_MASK) == 0) {
                            local_ids.push_back(value.get());
                        }
                        local_samples.push_back(elapsed_ns(t_begin, steady_clock::now()));
                        ++local_pop;
                    }
                } else {
                    auto value = q->try_pop();
                    if (value.has_value()) {
                        if ((value.get() & ID_SAMPLE_MASK) == 0) {
                            local_ids.push_back(value.get());
                        }
                        ++local_pop;
                    }
                }
            }
            pop_cnt[i].value = local_pop;
            pop_samples[i] = std::move(local_samples);
            sampled_ids[i] = std::move(local_ids);
        });
    }

    auto t0 = steady_clock::now();
    start.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::seconds(duration_sec));
    stop.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    uint64_t drained = 0;
    while (true) {
        auto value = q->try_pop();
        if (!value.has_value()) {
            break;
        }
        if ((value.get() & ID_SAMPLE_MASK) == 0) {
            sampled_ids[0].push_back(value.get());
        }
        ++drained;
    }

    auto t1 = steady_clock::now();
    const auto merged_ids = merge_id_samples(sampled_ids);
    const bool duplicate_sample = has_duplicate_sample(merged_ids);
    print_result("mpmc", sum_slots(push_cnt), sum_slots(pop_cnt), drained, duration_cast<milliseconds>(t1 - t0),
                 summarize_latency(merge_samples(push_samples)),
                 summarize_latency(merge_samples(pop_samples)),
                 merged_ids.size(),
                 duplicate_sample);
    require_balanced("mpmc", sum_slots(push_cnt), sum_slots(pop_cnt), drained);
    require_no_duplicates("mpmc", duplicate_sample);
}

// SPMC 测试
void test_spmc(int stealers, int duration_sec) {
    std::cout << "\n=== SPMC Deque (1P" << stealers << "C) ===\n";

    // 必须在 owner 线程创建
    auto q = std::make_unique<spmc_deque<uint64_t, CAPACITY>>();
    std::atomic<bool> start{false}, stop{false};
    std::vector<counter_slot> pop_cnt(stealers);
    std::vector<latency_samples> pop_samples(stealers);
    std::vector<id_samples> sampled_ids(stealers);

    std::vector<std::thread> threads;

    // Stealer threads (consumers)
    for (int i = 0; i < stealers; ++i) {
        threads.emplace_back([&, i] {
            bind_current_thread(static_cast<size_t>(i + 1));
            wait_for_start(start);
            uint64_t local_pop = 0;
            uint64_t sample_ticket = 0;
            latency_samples local_samples;
            local_samples.reserve(16384);
            id_samples local_ids;
            local_ids.reserve(8192);
            while (!stop.load(std::memory_order_acquire)) {
                if (should_sample(sample_ticket++)) {
                    const auto t_begin = steady_clock::now();
                    auto value = q->try_pop_front();
                    if (value.has_value()) {
                        if ((value.get() & ID_SAMPLE_MASK) == 0) {
                            local_ids.push_back(value.get());
                        }
                        local_samples.push_back(elapsed_ns(t_begin, steady_clock::now()));
                        ++local_pop;
                    }
                } else {
                    auto value = q->try_pop_front();
                    if (value.has_value()) {
                        if ((value.get() & ID_SAMPLE_MASK) == 0) {
                            local_ids.push_back(value.get());
                        }
                        ++local_pop;
                    }
                }
            }
            pop_cnt[i].value = local_pop;
            pop_samples[i] = std::move(local_samples);
            sampled_ids[i] = std::move(local_ids);
        });
    }

    // Owner thread (producer) - must be the main thread
    bind_current_thread(0);
    auto t0 = steady_clock::now();
    start.store(true, std::memory_order_release);

    uint64_t id = 0;
    uint64_t push_cnt = 0;
    uint64_t sample_ticket = 0;
    latency_samples push_samples;
    push_samples.reserve(32768);
    const auto deadline = t0 + std::chrono::seconds(duration_sec);
    while (steady_clock::now() < deadline) {
        if (should_sample(sample_ticket++)) {
            const auto t_begin = steady_clock::now();
            if (q->try_emplace(uint64_t{id})) {
                push_samples.push_back(elapsed_ns(t_begin, steady_clock::now()));
                ++id;
                ++push_cnt;
            }
        } else if (q->try_emplace(uint64_t{id})) {
            ++id;
            ++push_cnt;
        }
    }
    stop.store(true, std::memory_order_release);

    for (auto& t : threads) t.join();

    // Drain
    uint64_t drained = 0;
    while (true) {
        auto value = q->try_pop_back();
        if (!value.has_value()) {
            break;
        }
        if ((value.get() & ID_SAMPLE_MASK) == 0) {
            sampled_ids[0].push_back(value.get());
        }
        ++drained;
    }

    auto t1 = steady_clock::now();
    const auto merged_ids = merge_id_samples(sampled_ids);
    const bool duplicate_sample = has_duplicate_sample(merged_ids);
    print_result("spmc", push_cnt, sum_slots(pop_cnt), drained, duration_cast<milliseconds>(t1 - t0),
                 summarize_latency(std::move(push_samples)),
                 summarize_latency(merge_samples(pop_samples)),
                 merged_ids.size(),
                 duplicate_sample);
    require_balanced("spmc", push_cnt, sum_slots(pop_cnt), drained);
    require_no_duplicates("spmc", duplicate_sample);
}

int main(int argc, char** argv) {
    const int duration_sec = argc > 1 ? parse_positive_or_default(argv[1], DEFAULT_DURATION_SEC)
                                      : DEFAULT_DURATION_SEC;

    std::cout << "=== flux_foundry Queue Benchmarks ===\n";
    std::cout << "Capacity: " << CAPACITY
              << " Duration: " << duration_sec << "s"
              << " LatencySampleStride: " << LATENCY_SAMPLE_STRIDE << '\n';

    test_spsc(duration_sec);
    test_mpsc(4, duration_sec);
    test_mpsc(8, duration_sec);
    test_mpmc(4, 4, duration_sec);
    test_mpmc(8, 8, duration_sec);
    test_spmc(1, duration_sec);
    test_spmc(2, duration_sec);
    test_spmc(4, duration_sec);
    test_spmc(8, duration_sec);

    std::cout << "\n=== All tests completed ===\n";
    return 0;
}
