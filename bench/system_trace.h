#ifndef FF_BENCH_SYSTEM_TRACE_H
#define FF_BENCH_SYSTEM_TRACE_H

#ifndef FF_BENCH_SYSTEM_TRACE
#define FF_BENCH_SYSTEM_TRACE 0
#endif
static_assert(FF_BENCH_SYSTEM_TRACE == 0 || FF_BENCH_SYSTEM_TRACE == 1,
              "FF_BENCH_SYSTEM_TRACE must be 0 or 1");

#if FF_BENCH_SYSTEM_TRACE
#ifndef __APPLE__
#error "FF_BENCH_SYSTEM_TRACE requires macOS"
#endif
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <mach/mach_time.h>
#include <os/log.h>
#include <os/signpost.h>
#include <pthread.h>
#include <unistd.h>
#endif

namespace ff_bench {
#if FF_BENCH_SYSTEM_TRACE
inline void name_trace_thread(const char* name) {
    if (pthread_setname_np(name) != 0) std::abort();
}
inline unsigned long long trace_thread_id() noexcept {
    std::uint64_t tid = 0;
    if (pthread_threadid_np(nullptr, &tid)) std::abort();
    return static_cast<unsigned long long>(tid);
}
inline void await_trace_attachment(unsigned long long consumer_tid) {
    const char* ready = std::getenv("FF_BENCH_READY_FILE");
    const char* go = std::getenv("FF_BENCH_GO_FILE");
    if (!ready && !go) return;
    if (!ready || !*ready || !go || !*go || !consumer_tid) std::abort();
    const auto temporary = std::string(ready) + ".tmp";
    auto* file = std::fopen(temporary.c_str(), "w");
    if (!file) { std::perror(temporary.c_str()); std::abort(); }
    std::fputs("pid,producer_tid,consumer_tid\n", file);
    std::fprintf(file, "%ld,%llu,%llu\n", static_cast<long>(getpid()), trace_thread_id(), consumer_tid);
    bool ok = !std::ferror(file);
    if (std::fclose(file)) ok = false;
    if (!ok || std::rename(temporary.c_str(), ready)) std::abort();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (std::chrono::steady_clock::now() < deadline) {
        file = std::fopen(go, "r");
        if (file) { std::fclose(file); return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::fprintf(stderr, "timed out waiting for profiler attachment\n");
    std::abort();
}
class SystemTraceRegion {
    bool active_;
    os_log_t log_ = nullptr;
    os_signpost_id_t id_ = OS_SIGNPOST_ID_INVALID;
    std::uint64_t start_ticks_ = 0, end_ticks_ = 0;
    std::int64_t origin_ = 0, start_ns_ = 0, end_ns_ = 0;
    std::int64_t start_epoch_ns_ = 0, end_epoch_ns_ = 0;
    static std::int64_t now_ns() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
public:
    explicit SystemTraceRegion(bool active) : active_(active) {
        if (active_) {
            // Initialize the logging subsystem outside measurement.
            static const auto log = os_log_create("org.flux-foundry.bench", OS_LOG_CATEGORY_POINTS_OF_INTEREST);
            log_ = log;
            id_ = os_signpost_id_generate(log_);
        }
    }
    SystemTraceRegion(const SystemTraceRegion&) = delete;
    SystemTraceRegion& operator=(const SystemTraceRegion&) = delete;
    void begin(std::int64_t origin) noexcept {
        if (!active_) return;
        origin_ = origin;
        start_ns_ = now_ns();
        start_epoch_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        start_ticks_ = mach_absolute_time();
        os_signpost_interval_begin(log_, id_, "FF measured", "origin_ns=%{public}lld", static_cast<long long>(origin));
    }
    void end() noexcept {
        if (!active_) return;
        os_signpost_interval_end(log_, id_, "FF measured");
        end_ticks_ = mach_absolute_time();
        end_ns_ = now_ns();
        end_epoch_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    void report() const {
        const char* path = std::getenv("FF_BENCH_PHASE_FILE");
        if (!active_ || !path || !*path) return;
        mach_timebase_info_data_t timebase{};
        std::uint64_t thread = 0;
        if (mach_timebase_info(&timebase) != KERN_SUCCESS || pthread_threadid_np(nullptr, &thread)) std::abort();
        auto* file = std::fopen(path, "w");
        if (!file) { std::perror(path); std::abort(); }
        std::fputs("pid,producer_tid,origin_ns,start_ns,end_ns,start_mach_ticks,end_mach_ticks,timebase_numer,timebase_denom,start_epoch_ns,end_epoch_ns\n", file);
        std::fprintf(file, "%ld,%llu,%lld,%lld,%lld,%llu,%llu,%u,%u,%lld,%lld\n",
            static_cast<long>(getpid()), static_cast<unsigned long long>(thread),
            static_cast<long long>(origin_), static_cast<long long>(start_ns_), static_cast<long long>(end_ns_),
            static_cast<unsigned long long>(start_ticks_), static_cast<unsigned long long>(end_ticks_),
            timebase.numer, timebase.denom,
            static_cast<long long>(start_epoch_ns_), static_cast<long long>(end_epoch_ns_));
        bool ok = !std::ferror(file);
        if (std::fclose(file)) ok = false;
        if (!ok) { std::fprintf(stderr, "cannot write phase metadata: %s\n", path); std::abort(); }
    }
};
#else
inline void name_trace_thread(const char*) {}
inline unsigned long long trace_thread_id() noexcept { return 0; }
inline void await_trace_attachment(unsigned long long) {}
struct SystemTraceRegion {
    explicit SystemTraceRegion(bool) {}
    void begin(long long) noexcept {}
    void end() noexcept {}
    void report() const {}
};
#endif
}
#endif
