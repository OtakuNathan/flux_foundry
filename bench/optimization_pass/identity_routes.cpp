// Build timing and histogram binaries separately: defining
// FF_IDENTITY_ROUTES_TRACE adds a diagnostic hook, so those runs are not timings.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef FF_IDENTITY_ROUTES_TRACE
namespace identity_routes {
struct trace_state {
    bool enabled = false;
    std::uint64_t histogram[17]{};
    std::uint64_t published = 0;
    std::uint64_t adopted = 0;
};
static trace_state trace;
inline void observe(size_t, size_t count, bool published) noexcept {
    if (!trace.enabled) return;
    if (!count || count > 16) std::abort();
    ++trace.histogram[count];
    if (published) ++trace.published;
    else ++trace.adopted;
}
} // namespace identity_routes

#define FLUX_FOUNDRY_IDENTITY_POOL_TRACE_BATCH(origin, count, published) \
    ::identity_routes::observe((origin), (count), (published))
#endif
#include "memory/identity_pool.h"

namespace {
struct options {
    size_t routes = 1;
    size_t owners = 4;
    size_t blocks = 128;
    size_t rounds = 200;
    size_t warmup = 20;
    bool interleaved = true;
};

void check(bool passed, const char* what) {
    if (!passed) { std::fprintf(stderr, "FAIL: %s\n", what); std::abort(); }
}

size_t number(const char* text) {
    char* end = nullptr;
    const auto value = std::strtoull(text, &end, 10);
    check(text[0] && end && !*end, "expected an unsigned integer");
    return static_cast<size_t>(value);
}

options parse(int argc, char** argv) {
    options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string flag(argv[i]);
        if (flag == "--help") {
            std::puts("identity_routes --routes 1|4 --owners 1..8 --blocks 1..256 "
                      "--rounds N --warmup N --order grouped|interleaved\n"
                      "One releasing thread returns blocks to live allocating owners. "
                      "Only the deallocation/explicit-flush loop is timed; allocation, "
                      "payload checks, barriers and thread teardown are outside it.\n"
                      "Compile with FF_IDENTITY_ROUTES_TRACE for an untimed histogram run.");
            std::exit(0);
        }
        check(i + 1 < argc, "option is missing its value");
        const char* value = argv[++i];
        if (flag == "--routes") opt.routes = number(value);
        else if (flag == "--owners") opt.owners = number(value);
        else if (flag == "--blocks") opt.blocks = number(value);
        else if (flag == "--rounds") opt.rounds = number(value);
        else if (flag == "--warmup") opt.warmup = number(value);
        else if (flag == "--order") {
            check(std::strcmp(value, "grouped") == 0 || std::strcmp(value, "interleaved") == 0,
                "unknown release order");
            opt.interleaved = std::strcmp(value, "interleaved") == 0;
        } else check(false, "unknown option");
    }
    check(opt.routes == 1 || opt.routes == 4, "routes must be 1 or 4");
    check(opt.owners > 0 && opt.owners <= 8, "owners must be between 1 and 8");
    check(opt.blocks > 0 && opt.blocks <= 256, "blocks must fit each 256-slot owner inbox");
    check(opt.rounds > 0 && opt.rounds <= 1000000 && opt.warmup <= 1000000,
        "round count must be bounded and measured rounds nonzero");
    return opt;
}

unsigned char payload(size_t round, size_t owner, size_t block) {
    return static_cast<unsigned char>((round * 11 + owner * 17 + block) % 251);
}

#ifndef FF_IDENTITY_ROUTES_TRACE
double percentile(const std::vector<double>& sorted, double fraction) {
    const auto index = static_cast<size_t>(fraction * static_cast<double>(sorted.size() - 1));
    return sorted[index];
}
#endif

template <size_t Routes>
void run(const options& opt) {
    using pool = flux_foundry::identity_pool_allocator<96, 8, 256, 16, 64, 256, Routes>;
    struct owner_data {
        size_t identity = 0;
        std::vector<void*> blocks;
    };
    std::vector<owner_data> owners(opt.owners);
    for (auto& owner : owners) owner.blocks.resize(opt.blocks);
    std::mutex mutex;
    std::condition_variable changed;
    size_t ready = 0, released_rounds = 0;
    const auto total_rounds = opt.rounds + opt.warmup;
    std::vector<std::thread> workers;
    for (size_t owner = 0; owner < opt.owners; ++owner) workers.emplace_back([&, owner] {
        auto& data = owners[owner];
        data.identity = pool::current_identity();
        check(data.identity != 0, "owner registration failed");
        for (size_t round = 0; round < total_rounds; ++round) {
            for (size_t block = 0; block < opt.blocks; ++block) {
                auto* p = pool::alloc();
                check(p != nullptr, "allocation failed");
                data.blocks[block] = p;
                std::memset(p, payload(round, owner, block), 96);
            }
            std::unique_lock<std::mutex> lock(mutex);
            ++ready;
            changed.notify_all();
            changed.wait(lock, [&] { return released_rounds > round; });
        }
    });

    std::vector<double> elapsed;
    elapsed.reserve(opt.rounds);
    std::uint64_t checksum = 0, expected_checksum = 0, checked_blocks = 0;
    for (size_t round = 0; round < total_rounds; ++round) {
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&] { return ready == (round + 1) * opt.owners; });
        }
        for (size_t owner = 0; owner < opt.owners; ++owner) {
            for (size_t block = 0; block < opt.blocks; ++block) {
                const auto value = payload(round, owner, block);
                const auto* bytes = static_cast<const unsigned char*>(owners[owner].blocks[block]);
                for (size_t byte = 0; byte < 96; ++byte) {
                    check(bytes[byte] == value, "payload was corrupted or reused before release");
                    checksum += bytes[byte];
                }
                expected_checksum += std::uint64_t(value) * 96;
                ++checked_blocks;
            }
        }

#ifdef FF_IDENTITY_ROUTES_TRACE
        identity_routes::trace.enabled = round >= opt.warmup;
#else
        const auto start = std::chrono::steady_clock::now();
#endif
        if (opt.interleaved) {
            for (size_t block = 0; block < opt.blocks; ++block)
                for (auto& owner : owners) pool::dealloc(owner.identity, owner.blocks[block]);
        } else {
            for (auto& owner : owners)
                for (auto* p : owner.blocks) pool::dealloc(owner.identity, p);
        }
        pool::flush_remote_returns();
#ifndef FF_IDENTITY_ROUTES_TRACE
        const auto stop = std::chrono::steady_clock::now();
        if (round >= opt.warmup) {
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
            elapsed.push_back(static_cast<double>(ns) / static_cast<double>(opt.owners * opt.blocks));
        }
#endif
        {
            std::lock_guard<std::mutex> lock(mutex);
            released_rounds = round + 1;
        }
        changed.notify_all();
    }
    for (auto& worker : workers) worker.join();
    check(checksum == expected_checksum && checked_blocks == total_rounds * opt.owners * opt.blocks,
        "final checksum or block count mismatch");
    std::printf("routes,owners,order,blocks_per_owner,rounds,warmup,checked_blocks,checksum");
#ifdef FF_IDENTITY_ROUTES_TRACE
    std::puts(",mode,flushes,published,adopted,batch1,batch16,mean_batch");
    std::uint64_t flushes = 0, traced_blocks = 0;
    for (size_t count = 1; count <= 16; ++count) {
        flushes += identity_routes::trace.histogram[count];
        traced_blocks += count * identity_routes::trace.histogram[count];
    }
    check(traced_blocks == opt.rounds * opt.owners * opt.blocks, "batch histogram lost blocks");
    check(identity_routes::trace.adopted == 0, "benchmark unexpectedly exceeded an owner inbox");
    std::printf("%zu,%zu,%s,%zu,%zu,%zu,%llu,%llu,histogram,%llu,%llu,%llu,%llu,%llu,%.6f\n",
        Routes, opt.owners, opt.interleaved ? "interleaved" : "grouped", opt.blocks, opt.rounds, opt.warmup,
        static_cast<unsigned long long>(checked_blocks), static_cast<unsigned long long>(checksum),
        static_cast<unsigned long long>(flushes), static_cast<unsigned long long>(identity_routes::trace.published),
        static_cast<unsigned long long>(identity_routes::trace.adopted),
        static_cast<unsigned long long>(identity_routes::trace.histogram[1]),
        static_cast<unsigned long long>(identity_routes::trace.histogram[16]),
        static_cast<double>(traced_blocks) / static_cast<double>(flushes));
    std::fputs("histogram", stderr);
    for (size_t count = 1; count <= 16; ++count)
        std::fprintf(stderr, " %zu:%llu", count,
            static_cast<unsigned long long>(identity_routes::trace.histogram[count]));
    std::fputc('\n', stderr);
#else
    std::puts(",mode,median_ns_per_release,p99_ns_per_release,min_ns_per_release,max_ns_per_release");
    std::sort(elapsed.begin(), elapsed.end());
    const auto middle = elapsed.size() / 2;
    const double median = elapsed.size() % 2 ? elapsed[middle] : (elapsed[middle - 1] + elapsed[middle]) / 2;
    std::printf("%zu,%zu,%s,%zu,%zu,%zu,%llu,%llu,timing,%.6f,%.6f,%.6f,%.6f\n",
        Routes, opt.owners, opt.interleaved ? "interleaved" : "grouped", opt.blocks, opt.rounds, opt.warmup,
        static_cast<unsigned long long>(checked_blocks), static_cast<unsigned long long>(checksum),
        median, percentile(elapsed, 0.99), elapsed.front(), elapsed.back());
#endif
}
} // namespace

int main(int argc, char** argv) {
    const auto opt = parse(argc, argv);
    if (opt.routes == 1) run<1>(opt);
    else run<4>(opt);
}
