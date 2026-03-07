#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
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
#include "utility/static_stack.h"

using namespace flux_foundry;
using namespace std::chrono;

constexpr size_t CAPACITY = (1u << 20);
using stack_t_ = static_stack<uint64_t, CAPACITY>;

constexpr int DEFAULT_PRODUCERS = 8;
constexpr int DEFAULT_CONSUMERS = 8;
constexpr int DEFAULT_DURATION_SEC = 5;
constexpr size_t LATENCY_SAMPLE_STRIDE = 4096;
constexpr uint64_t ID_SAMPLE_MASK = (1u << 13) - 1;

static_assert((LATENCY_SAMPLE_STRIDE & (LATENCY_SAMPLE_STRIDE - 1)) == 0,
              "latency sample stride must be power-of-two");

struct alignas(OPTIMIZED_ALIGN) counter_slot {
    uint64_t value = 0;
};

using latency_samples = std::vector<uint32_t>;
using id_samples = std::vector<uint64_t>;

struct latency_summary {
    size_t samples = 0;
    uint32_t p50_ns = 0;
    uint32_t p99_ns = 0;
    uint32_t max_ns = 0;
};

template <typename Slots>
uint64_t sum_slots(const Slots& slots) noexcept {
    uint64_t total = 0;
    for (const auto& slot : slots) {
        total += slot.value;
    }
    return total;
}

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
latency_samples merge_latency_samples(const SamplesByThread& samples_by_thread) {
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

void wait_for_start(const std::atomic<bool>& start) noexcept {
    backoff_strategy<> backoff;
    while (!start.load(std::memory_order_acquire)) {
        backoff.yield();
    }
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

int main(int argc, char** argv) {
    const int producers = argc > 1 ? parse_positive_or_default(argv[1], DEFAULT_PRODUCERS) : DEFAULT_PRODUCERS;
    const int consumers = argc > 2 ? parse_positive_or_default(argv[2], DEFAULT_CONSUMERS) : DEFAULT_CONSUMERS;
    const int duration_sec = argc > 3 ? parse_positive_or_default(argv[3], DEFAULT_DURATION_SEC) : DEFAULT_DURATION_SEC;

    std::cout << "=== flux_foundry static_stack benchmark ===\n";
    std::cout << "capacity=" << CAPACITY
              << " producers=" << producers
              << " consumers=" << consumers
              << " duration=" << duration_sec << "s"
              << " latency_stride=" << LATENCY_SAMPLE_STRIDE
              << " id_sample_mask=0x" << std::hex << ID_SAMPLE_MASK << std::dec << '\n';

    auto stk = std::make_unique<stack_t_>();

    std::atomic<bool> start{false};
    std::atomic<bool> stop{false};

    std::vector<counter_slot> push_cnt(producers);
    std::vector<counter_slot> pop_cnt(consumers);
    std::vector<latency_samples> push_latency(producers);
    std::vector<latency_samples> pop_latency(consumers);
    std::vector<id_samples> sampled_ids(consumers);

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(producers + consumers));

    for (int i = 0; i < producers; ++i) {
        threads.emplace_back([&, i] {
            bind_current_thread(static_cast<size_t>(i));
            wait_for_start(start);

            uint64_t local_push = 0;
            uint64_t sample_ticket = 0;
            latency_samples local_latency;
            local_latency.reserve(16384);
            backoff_strategy<> backoff;

            while (!stop.load(std::memory_order_acquire)) {
                const uint64_t id = local_push * static_cast<uint64_t>(producers) + static_cast<uint64_t>(i);
                if (should_sample(sample_ticket++)) {
                    const auto t_begin = steady_clock::now();
                    if (stk->emplace(id)) {
                        local_latency.push_back(elapsed_ns(t_begin, steady_clock::now()));
                        ++local_push;
                        backoff.reset();
                    } else {
                        backoff.yield();
                    }
                } else if (stk->emplace(id)) {
                    ++local_push;
                    backoff.reset();
                } else {
                    backoff.yield();
                }
            }

            push_cnt[i].value = local_push;
            push_latency[i] = std::move(local_latency);
        });
    }

    for (int i = 0; i < consumers; ++i) {
        threads.emplace_back([&, i] {
            bind_current_thread(static_cast<size_t>(producers + i));
            wait_for_start(start);

            uint64_t local_pop = 0;
            uint64_t sample_ticket = 0;
            latency_samples local_latency;
            id_samples local_ids;
            local_latency.reserve(16384);
            local_ids.reserve(8192);
            backoff_strategy<> backoff;

            while (!stop.load(std::memory_order_acquire)) {
                if (should_sample(sample_ticket++)) {
                    const auto t_begin = steady_clock::now();
                    auto value = stk->pop();
                    if (value.has_value()) {
                        const uint64_t id = value.get();
                        local_latency.push_back(elapsed_ns(t_begin, steady_clock::now()));
                        if ((id & ID_SAMPLE_MASK) == 0) {
                            local_ids.push_back(id);
                        }
                        ++local_pop;
                        backoff.reset();
                    } else {
                        backoff.yield();
                    }
                } else {
                    auto value = stk->pop();
                    if (value.has_value()) {
                        const uint64_t id = value.get();
                        if ((id & ID_SAMPLE_MASK) == 0) {
                            local_ids.push_back(id);
                        }
                        ++local_pop;
                        backoff.reset();
                    } else {
                        backoff.yield();
                    }
                }
            }

            pop_cnt[i].value = local_pop;
            pop_latency[i] = std::move(local_latency);
            sampled_ids[i] = std::move(local_ids);
        });
    }

    const auto t0 = steady_clock::now();
    start.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::seconds(duration_sec));
    stop.store(true, std::memory_order_release);

    for (auto& thread : threads) {
        thread.join();
    }

    uint64_t drained = 0;
    while (true) {
        auto value = stk->pop();
        if (!value.has_value()) {
            break;
        }
        const uint64_t id = value.get();
        if ((id & ID_SAMPLE_MASK) == 0) {
            sampled_ids[0].push_back(id);
        }
        ++drained;
    }

    const auto t1 = steady_clock::now();
    const auto elapsed = duration_cast<milliseconds>(t1 - t0);

    const uint64_t pushed = sum_slots(push_cnt);
    const uint64_t popped_during_run = sum_slots(pop_cnt);
    const uint64_t popped_total = popped_during_run + drained;

    const auto push_summary = summarize_latency(merge_latency_samples(push_latency));
    const auto pop_summary = summarize_latency(merge_latency_samples(pop_latency));
    const auto merged_ids = merge_id_samples(sampled_ids);
    const bool duplicate_sample = has_duplicate_sample(merged_ids);

    const double seconds = static_cast<double>(elapsed.count()) / 1000.0;
    const double throughput = seconds > 0.0 ? static_cast<double>(popped_total) / seconds / 1000000.0 : 0.0;

    std::cout << "push=" << pushed
              << " pop=" << popped_during_run
              << " drain=" << drained
              << " elapsed_ms=" << elapsed.count()
              << " throughput=" << throughput << " Mops/s";
    print_latency("push", push_summary);
    print_latency("pop", pop_summary);
    std::cout << " sampled_ids=" << merged_ids.size()
              << " duplicate_sample=" << (duplicate_sample ? "yes" : "no")
              << '\n';

    if (duplicate_sample) {
        std::cerr << "duplicate sampled pop detected\n";
        return 1;
    }
    if (pushed != popped_total) {
        std::cerr << "push/pop mismatch: push=" << pushed << " pop=" << popped_total << '\n';
        return 1;
    }

    return 0;
}
