#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "memory/pooling.h"

namespace {

using clock_t = std::chrono::steady_clock;

struct run_cfg {
    int rounds = 200;
    int threads = 8;
    int ops_per_round = 64;
    int collect_latency = 1;
};

struct run_stat {
    uint64_t alloc_ops = 0;
    uint64_t dealloc_ops = 0;
    uint64_t alloc_fail = 0;
    uint64_t align_fail = 0;
    uint64_t tag_fail = 0;
    uint64_t alloc_p50_ns = 0;
    uint64_t alloc_p90_ns = 0;
    uint64_t alloc_p99_ns = 0;
    uint64_t alloc_max_ns = 0;
    uint64_t dealloc_p50_ns = 0;
    uint64_t dealloc_p90_ns = 0;
    uint64_t dealloc_p99_ns = 0;
    uint64_t dealloc_max_ns = 0;
};

constexpr uint64_t k_live_magic = 0xF10CF10CF10CF10CULL;
constexpr uint64_t k_dead_magic = 0xDEADDEADDEADDEADULL;

bool parse_positive_int(const char* s, int& out) {
    if (!s || !*s) {
        return false;
    }
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (*end != '\0' || v <= 0 || v > 100000000L) {
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

bool parse_nonnegative_int(const char* s, int& out) {
    if (!s || !*s) {
        return false;
    }
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (*end != '\0' || v < 0 || v > 100000000L) {
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

void print_usage(const char* exe) {
    std::printf(
        "usage: %s [rounds] [threads] [ops_per_round] [collect_latency]\n"
        "defaults: rounds=200 threads=8 ops_per_round=64 collect_latency=1\n",
        exe);
}

template <size_t Size, size_t Align>
struct slot_io {
    static_assert(Size >= sizeof(uint64_t) * 2, "Size must be >= 16");

    static void write_live(void* p, uint32_t tid, uint32_t seq) noexcept {
        auto* u = static_cast<uint64_t*>(p);
        u[0] = k_live_magic;
        u[1] = (static_cast<uint64_t>(tid) << 32) | static_cast<uint64_t>(seq);
    }

    static bool validate_live_and_mark_dead(void* p) noexcept {
        auto* u = static_cast<uint64_t*>(p);
        if (u[0] != k_live_magic) {
            return false;
        }
        u[0] = k_dead_magic;
        return true;
    }
};

template <size_t Size, size_t Align>
run_stat run_case(const run_cfg& cfg, const char* tag) {
    using allocator_t = flux_foundry::flux_foundry_allocator<Size, Align>;

    std::atomic<int> ready{0};
    std::atomic<int> go{0};
    std::atomic<uint64_t> alloc_ops{0};
    std::atomic<uint64_t> dealloc_ops{0};
    std::atomic<uint64_t> alloc_fail{0};
    std::atomic<uint64_t> align_fail{0};
    std::atomic<uint64_t> tag_fail{0};

    std::vector<std::vector<uint32_t>> alloc_samples_by_thread(
        static_cast<size_t>(cfg.threads));
    std::vector<std::vector<uint32_t>> dealloc_samples_by_thread(
        static_cast<size_t>(cfg.threads));
    if (cfg.collect_latency) {
        const size_t reserve_n =
            static_cast<size_t>(cfg.rounds) * static_cast<size_t>(cfg.ops_per_round);
        for (auto& v : alloc_samples_by_thread) {
            v.reserve(reserve_n);
        }
        for (auto& v : dealloc_samples_by_thread) {
            v.reserve(reserve_n);
        }
    }

    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(cfg.threads));

    for (int tid = 0; tid < cfg.threads; ++tid) {
        workers.emplace_back([&, tid]() {
            allocator_t allocator;
            auto& alloc_samples = alloc_samples_by_thread[static_cast<size_t>(tid)];
            auto& dealloc_samples = dealloc_samples_by_thread[static_cast<size_t>(tid)];
            std::vector<void*> live;
            live.reserve(static_cast<size_t>(cfg.ops_per_round));

            ready.fetch_add(1, std::memory_order_release);
            while (go.load(std::memory_order_acquire) == 0) {
                std::this_thread::yield();
            }

            uint32_t seq = 1;
            for (int r = 0; r < cfg.rounds; ++r) {
                live.clear();

                for (int i = 0; i < cfg.ops_per_round; ++i) {
                    const auto t0 = clock_t::now();
                    void* p = allocator.alloc();
                    const auto t1 = clock_t::now();

                    if (cfg.collect_latency) {
                        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                        alloc_samples.emplace_back(static_cast<uint32_t>(ns < 0 ? 0 : ns));
                    }

                    if (!p) {
                        alloc_fail.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }

                    alloc_ops.fetch_add(1, std::memory_order_relaxed);

                    if ((reinterpret_cast<uintptr_t>(p) & (Align - 1)) != 0) {
                        align_fail.fetch_add(1, std::memory_order_relaxed);
                        allocator.dealloc(p);
                        continue;
                    }

                    slot_io<Size, Align>::write_live(
                        p, static_cast<uint32_t>(tid), seq++);
                    live.push_back(p);
                }

                for (void* p : live) {
                    if (!slot_io<Size, Align>::validate_live_and_mark_dead(p)) {
                        tag_fail.fetch_add(1, std::memory_order_relaxed);
                    }

                    const auto t0 = clock_t::now();
                    allocator.dealloc(p);
                    const auto t1 = clock_t::now();

                    dealloc_ops.fetch_add(1, std::memory_order_relaxed);

                    if (cfg.collect_latency) {
                        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                        dealloc_samples.emplace_back(static_cast<uint32_t>(ns < 0 ? 0 : ns));
                    }
                }
            }
        });
    }

    while (ready.load(std::memory_order_acquire) != cfg.threads) {
        std::this_thread::yield();
    }
    go.store(1, std::memory_order_release);

    for (auto& t : workers) {
        t.join();
    }

    run_stat st{};
    st.alloc_ops = alloc_ops.load(std::memory_order_relaxed);
    st.dealloc_ops = dealloc_ops.load(std::memory_order_relaxed);
    st.alloc_fail = alloc_fail.load(std::memory_order_relaxed);
    st.align_fail = align_fail.load(std::memory_order_relaxed);
    st.tag_fail = tag_fail.load(std::memory_order_relaxed);

    if (cfg.collect_latency) {
        auto summarize = [](std::vector<std::vector<uint32_t>>& src,
                            uint64_t& p50, uint64_t& p90, uint64_t& p99, uint64_t& mx) {
            std::vector<uint32_t> merged;
            size_t total = 0;
            for (const auto& v : src) {
                total += v.size();
            }
            merged.reserve(total);
            for (auto& v : src) {
                merged.insert(merged.end(), v.begin(), v.end());
            }
            if (merged.empty()) {
                p50 = p90 = p99 = mx = 0;
                return;
            }
            std::sort(merged.begin(), merged.end());
            const size_t n = merged.size();
            p50 = merged[(n - 1) * 50 / 100];
            p90 = merged[(n - 1) * 90 / 100];
            p99 = merged[(n - 1) * 99 / 100];
            mx = merged.back();
        };

        summarize(
            alloc_samples_by_thread,
            st.alloc_p50_ns, st.alloc_p90_ns, st.alloc_p99_ns, st.alloc_max_ns);
        summarize(
            dealloc_samples_by_thread,
            st.dealloc_p50_ns, st.dealloc_p90_ns, st.dealloc_p99_ns, st.dealloc_max_ns);
    }

    std::printf(
        "[%s] alloc_ops=%" PRIu64 " dealloc_ops=%" PRIu64
        " alloc_fail=%" PRIu64 " align_fail=%" PRIu64 " tag_fail=%" PRIu64 "\n",
        tag, st.alloc_ops, st.dealloc_ops, st.alloc_fail, st.align_fail, st.tag_fail);

    if (cfg.collect_latency) {
        std::printf(
            "[%s] alloc latency(ns):   p50=%" PRIu64 " p90=%" PRIu64 " p99=%" PRIu64 " max=%" PRIu64 "\n",
            tag, st.alloc_p50_ns, st.alloc_p90_ns, st.alloc_p99_ns, st.alloc_max_ns);
        std::printf(
            "[%s] dealloc latency(ns): p50=%" PRIu64 " p90=%" PRIu64 " p99=%" PRIu64 " max=%" PRIu64 "\n",
            tag, st.dealloc_p50_ns, st.dealloc_p90_ns, st.dealloc_p99_ns, st.dealloc_max_ns);
    } else {
        std::printf("[%s] latency: disabled\n", tag);
    }

    return st;
}

}  // namespace

int main(int argc, char** argv) {
    run_cfg cfg{};
    if (argc > 1 && !parse_positive_int(argv[1], cfg.rounds)) {
        print_usage(argv[0]);
        return 2;
    }
    if (argc > 2 && !parse_positive_int(argv[2], cfg.threads)) {
        print_usage(argv[0]);
        return 2;
    }
    if (argc > 3 && !parse_positive_int(argv[3], cfg.ops_per_round)) {
        print_usage(argv[0]);
        return 2;
    }
    if (argc > 4 && !parse_nonnegative_int(argv[4], cfg.collect_latency)) {
        print_usage(argv[0]);
        return 2;
    }

    std::printf(
        "[pooling allocator stress]\n"
        "rounds=%d threads=%d ops_per_round=%d collect_latency=%d\n",
        cfg.rounds, cfg.threads, cfg.ops_per_round, cfg.collect_latency);

    auto low = run_case<64, 8>(cfg, "align8_path");
    auto high = run_case<96, 64>(cfg, "align64_path");

    const uint64_t fail_total =
        low.alloc_fail + low.align_fail + low.tag_fail +
        high.alloc_fail + high.align_fail + high.tag_fail;
    if (fail_total != 0) {
        std::printf("[FAIL] pooling allocator stress failed=%" PRIu64 "\n", fail_total);
        return 1;
    }

    std::printf("[PASS] pooling allocator stress passed\n");
    return 0;
}
