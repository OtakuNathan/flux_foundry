#ifndef FF_BENCH_LOAD_PROBE_H
#define FF_BENCH_LOAD_PROBE_H
// Bounded, scheduled-arrival load probe. Linux affinity is optional.
// Compile/run instructions and metric definitions: README.md in this directory.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <system_error>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif
#include "base/traits.h"
#include "flow/flow_def.h"
namespace flux_foundry {
template<> struct cancel_error<std::error_code> {
    static std::error_code make(cancel_kind) noexcept { return {1, std::generic_category()}; }
};
template<> struct awaitable_creating_error<std::error_code> {
    static std::error_code make() noexcept { return {2, std::generic_category()}; }
};
template<> struct async_submission_failed_error<std::error_code> {
    static std::error_code make() noexcept { return {3, std::generic_category()}; }
};
}
#include "flow/flow.h"
#include "executor/simple_executor.h"
#include "memory/aligned_alloc.h"
#include "bench/sample_trace.h"

namespace load_probe {
using namespace flux_foundry;
using Clock = std::chrono::steady_clock;
using Ns = std::int64_t;
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
using E = std::exception_ptr;
#else
using E = std::error_code;
#endif
static Ns now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
static void relax() noexcept {
#if defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    asm volatile("pause" ::: "memory");
#else
    std::this_thread::yield();
#endif
}
static void require(bool ok, const char* msg) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", msg); std::abort(); }
}
static void pin(int cpu) {
    if (cpu < 0) return;
#if defined(__linux__)
    require(cpu < CPU_SETSIZE, "CPU id exceeds CPU_SETSIZE");
    cpu_set_t cpus;
    CPU_ZERO(&cpus); CPU_SET(cpu, &cpus);
    require(pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus) == 0, "CPU affinity failed");
#else
    require(false, "CPU affinity only implemented for Linux; use -1");
#endif
}
struct Sample {
    Ns planned = 0, submit_start = 0, submit_end = 0;
    Ns dispatch_start = 0, dispatch_end = 0, begin = 0, end = 0;
    std::uint64_t value = 0;
    unsigned callbacks = 0;
    bool error = false;
};
struct Port {
    void* executor;
    void (*dispatch)(void*, task_wrapper_sbo&&) noexcept;
};
struct Context {
    Port port;
    std::vector<Sample>* samples;
    std::atomic<std::size_t> completed{0};
    unsigned work;
    std::size_t stall_index;
    Ns stall_ns;
    Context(Port p, std::vector<Sample>* s, unsigned w, std::size_t index, Ns stall)
        : port(p), samples(s), completed(0), work(w), stall_index(index), stall_ns(stall) {}
};
struct Event { Context* ctx; std::size_t id; std::uint64_t value; };
using R = result_t<Event, E>;
static std::uint64_t work_value(std::size_t id, unsigned loops) noexcept {
    std::uint64_t v = static_cast<std::uint64_t>(id) + UINT64_C(0x9e3779b97f4a7c15);
    for (unsigned j = 0; j < loops; ++j) {
        v ^= v >> 17; v *= UINT64_C(0xd6e8feb86659fd93); v ^= v << 11;
    }
    return v;
}
static Event work(Event e) noexcept {
    auto& s = (*e.ctx->samples)[e.id];
    s.begin = now_ns();
    if (e.id == e.ctx->stall_index && e.ctx->stall_ns > 0)
        std::this_thread::sleep_for(std::chrono::nanoseconds(e.ctx->stall_ns));
    e.value = work_value(e.id, e.ctx->work);
    return e;
}
static void finish(Event e) noexcept {
    auto& s = (*e.ctx->samples)[e.id];
    s.value = e.value;
    ++s.callbacks;
    s.end = now_ns();
    e.ctx->completed.fetch_add(1, std::memory_order_release);
}
static void enqueue(Event e, task_wrapper_sbo task) noexcept {
    auto& s = (*e.ctx->samples)[e.id];
    s.dispatch_start = now_ns();
    e.ctx->port.dispatch(e.ctx->port.executor, std::move(task));
    s.dispatch_end = now_ns();
}
struct Receiver {
    using value_type = R;
    void emplace(R&& r) noexcept {
        require(r.has_value(), "flow returned an error");
        finish(r.value());
    }
};
struct Normal final : awaitable_base<Normal, Event, E> {
    using async_result_type = R;
    Event e;
    explicit Normal(R&& r) noexcept : e(r.value()) {}
    bool available() const noexcept { return true; }
    int submit() noexcept {
        retain();
        auto* self = this;
        enqueue(e, task_wrapper_sbo([self]() noexcept {
            auto out = work(self->e);
            self->resume(R(value_tag, out));
            self->release();
        }));
        return 0;
    }
    void cancel() noexcept {}
};
struct Fast final : fast_awaitable_base<Fast, Event, E> {
    using async_result_type = R;
    Event e;
    explicit Fast(R&& r) noexcept : e(r.value()) {}
    bool available() const noexcept { return true; }
    int submit() noexcept {
        auto* self = this;
        enqueue(e, task_wrapper_sbo([self]() noexcept {
            auto out = work(self->e);
            self->resume(R(value_tag, out)); // final access to self
        }));
        return 0;
    }
    void cancel() noexcept {}
};
struct Profile { const char* name; std::uint64_t rate; std::size_t burst; Ns burst_period; Ns stall; };
static const Profile profiles[] = {
    {"steady100k", 100000, 0, 0, 0},
    {"steady500k", 500000, 0, 0, 0},
    {"overload2m", 2000000, 0, 0, 0},
    {"burst8192", 0, 8192, 20000000, 0},
    {"stall5ms", 500000, 0, 0, 5000000}
};
struct Options {
    std::size_t samples = 32768;
    unsigned repeats = 3, work = 128;
    int producer_cpu = -1, consumer_cpu = -1;
    const char* mode = "all";
    const char* profile = "all";
    unsigned capacity = 0;
};
static std::uint64_t number(const char* arg) {
    char* end = nullptr;
    auto n = std::strtoull(arg, &end, 10);
    require(arg[0] != '-' && end != arg && *end == '\0', "invalid nonnegative integer");
    return n;
}
static Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--help")) {
            std::puts("--samples N --repeats N --work N --capacity 256|1024|4096|16384\n"
                      "--mode all|raw|fast|normal --profile all|steady100k|steady500k|overload2m|burst8192|stall5ms\n"
                      "--producer-cpu N --consumer-cpu N (omit affinity options for portable unpinned run)");
            std::exit(0);
        }
        require(i + 1 < argc, "missing option value");
        const char* key = argv[i++]; const char* val = argv[i];
        if (!std::strcmp(key,"--samples")) o.samples = number(val);
        else if (!std::strcmp(key,"--repeats")) o.repeats = static_cast<unsigned>(number(val));
        else if (!std::strcmp(key,"--work")) o.work = static_cast<unsigned>(number(val));
        else if (!std::strcmp(key,"--capacity")) o.capacity = static_cast<unsigned>(number(val));
        else if (!std::strcmp(key,"--producer-cpu")) o.producer_cpu = static_cast<int>(number(val));
        else if (!std::strcmp(key,"--consumer-cpu")) o.consumer_cpu = static_cast<int>(number(val));
        else if (!std::strcmp(key,"--mode")) o.mode = val;
        else if (!std::strcmp(key,"--profile")) o.profile = val;
        else require(false,"unknown option");
    }
    require(o.samples > 0 && o.samples <= 1000000, "samples must be 1..1000000");
    require(o.repeats > 0 && o.repeats <= 20, "repeats must be 1..20");
    require(o.work <= 100000, "work must be <=100000");
    require(o.capacity == 0 || o.capacity == 256 || o.capacity == 1024 || o.capacity == 4096 || o.capacity == 16384, "invalid capacity");
    require(!std::strcmp(o.mode,"all") || !std::strcmp(o.mode,"raw") || !std::strcmp(o.mode,"fast") || !std::strcmp(o.mode,"normal"),"invalid mode");
    bool found = !std::strcmp(o.profile,"all");
    for (const auto& p : profiles) found = found || !std::strcmp(o.profile,p.name);
    require(found,"invalid profile");
    require(o.producer_cpu < 0 || o.consumer_cpu < 0 || o.producer_cpu != o.consumer_cpu, "use distinct CPUs for producer and consumer");
    return o;
}
static Ns due(const Profile& p, std::size_t i) noexcept {
    return p.burst ? static_cast<Ns>(i / p.burst) * p.burst_period
                   : static_cast<Ns>((static_cast<std::uint64_t>(i) * UINT64_C(1000000000)) / p.rate);
}
struct Quantiles { Ns p50, p95, p99, p999, max; };
static Quantiles quantiles(std::vector<Ns>& v) {
    std::sort(v.begin(),v.end());
    auto q = [&](unsigned permille) {
        const auto rank = (v.size() * permille + 999) / 1000;
        return v[std::max<std::size_t>(1,rank)-1];
    };
    return {q(500),q(950),q(990),q(999),v.back()};
}
struct CpuMonitor { void* state; Ns (*snapshot)(void*); };
struct ProbeTiming { Ns wall_ns, consumer_cpu_ns, process_cpu_ns; };
static ProbeTiming measured_run(Port port, std::size_t capacity, const Options& o, const Profile& p, const char* mode, unsigned repeat, const char* prefix = "", bool newline = true, const CpuMonitor* monitor = nullptr, const CpuMonitor* process_monitor = nullptr) {
    // Preallocate and touch all sample storage; blueprint setup is outside timing.
    std::vector<Sample> samples(o.samples);
    Context ctx(port, &samples, o.work, o.samples / 4, p.stall);
    auto nb = make_blueprint<Event,E>() | await<Normal>() | end();
    auto np = make_lite_ptr<decltype(nb)>(std::move(nb));
    auto fb = make_blueprint<Event,E>() | await<Fast>() | end();
    std::size_t peak_outstanding = 0;
    const Ns cpu_start = monitor ? monitor->snapshot(monitor->state) : 0;
    const Ns process_start = process_monitor ? process_monitor->snapshot(process_monitor->state) : 0;
    const Ns monitor_start = now_ns();
    const Ns origin = monitor_start + 2000000;
    Ns last_submit = origin;
    for (std::size_t i = 0; i < o.samples; ++i) {
        auto& s = samples[i];
        s.planned = origin + due(p,i);
        while (now_ns() < s.planned) relax();
        s.submit_start = now_ns();
        const auto outstanding = i + 1 - ctx.completed.load(std::memory_order_acquire);
        peak_outstanding = std::max(peak_outstanding,outstanding);
        Event e{&ctx,i,0};
        if (!std::strcmp(mode,"raw")) {
            enqueue(e, task_wrapper_sbo([e]() noexcept { finish(work(e)); }));
        } else if (!std::strcmp(mode,"fast")) {
            auto r = make_fast_runner_view(fb,Receiver{}); r(e);
        } else if (!std::strcmp(mode,"ordinary-fast")) {
            // Used explicitly by the production-uv handoff probe. Keep the
            // awaitable and owning blueprint identical to the normal lane.
            auto r = make_fast_runner(np,Receiver{}); r(e);
        } else {
            auto r = make_runner(np,Receiver{}); r(e);
        }
        s.submit_end = now_ns();
        last_submit = s.submit_end;
    }
    const Ns timeout = now_ns() + INT64_C(30000000000);
    while (ctx.completed.load(std::memory_order_acquire) != o.samples) {
        require(now_ns() < timeout,"completion drain timed out"); relax();
    }
    // Stop/drain/join occurs in the caller before samples/context are destroyed:
    // awaitable cleanup can still be running after Receiver publishes completion.
    // Send an ordered fence through this single-consumer FIFO before returning.
    std::atomic<bool> fenced{false};
    port.dispatch(port.executor,task_wrapper_sbo([&fenced]() noexcept { fenced.store(true,std::memory_order_release); }));
    while (!fenced.load(std::memory_order_acquire)) relax();

    const Ns monitor_end = now_ns();
    const Ns process_end = process_monitor ? process_monitor->snapshot(process_monitor->state) : 0;
    const Ns cpu_end = monitor ? monitor->snapshot(monitor->state) : 0;
    std::vector<Ns> e2e, actual, lag, dispatch, service;
    e2e.reserve(o.samples); actual.reserve(o.samples); lag.reserve(o.samples);
    dispatch.reserve(o.samples); service.reserve(o.samples);
    Ns last_end = origin;
    std::size_t late = 0;
    const Ns late_threshold = p.burst ? 1000000 : static_cast<Ns>(1000000000 / p.rate);
    for (std::size_t i = 0; i < o.samples; ++i) {
        const auto& s = samples[i];
        require(s.callbacks == 1 && !s.error,"not exactly one successful callback per event");
        require(s.value == work_value(i,o.work),"work checksum mismatch");
        require(s.end >= s.begin && s.begin >= s.dispatch_start && s.submit_start >= s.planned,"timestamp order invalid");
        require(s.dispatch_end >= s.dispatch_start && s.submit_end >= s.submit_start,"producer timestamps invalid");
        e2e.push_back(s.end - s.planned);
        actual.push_back(s.end - s.submit_start);
        lag.push_back(s.submit_start - s.planned);
        dispatch.push_back(s.dispatch_end - s.dispatch_start);
        service.push_back(s.end - s.begin);
        last_end = std::max(last_end,s.end);
        if (s.submit_start - s.planned > late_threshold) ++late;
    }
    const double elapsed_s = static_cast<double>(last_end-origin)/1e9;
    const Ns schedule_end = origin + due(p,o.samples-1);
    const Ns drain_last_submit = std::max<Ns>(0,last_end-last_submit);
    const Ns drain_schedule = std::max<Ns>(0,last_end-schedule_end);
    const auto qe = quantiles(e2e), qa = quantiles(actual), ql = quantiles(lag);
    const auto qd = quantiles(dispatch), qs = quantiles(service);
    std::printf("%s", prefix);
    std::printf("%zu,%s,%s,%u,%zu,%u,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%zu,%zu,%.3f,%.3f,%.3f%s",
        capacity, mode, p.name, repeat, o.samples, o.work,o.producer_cpu,o.consumer_cpu,
        o.samples/elapsed_s,
        qe.p50/1000.0,qe.p95/1000.0,qe.p99/1000.0,qe.p999/1000.0,qe.max/1000.0,
        qa.p99/1000.0,ql.p50/1000.0,ql.p99/1000.0,
        qd.p50/1000.0,qd.p99/1000.0,qd.max/1000.0,
        qs.p50/1000.0,peak_outstanding,late,
        drain_last_submit/1000000.0,drain_schedule/1000000.0,sizeof(Sample)*o.samples/1048576.0,newline ? "\n" : ",");
    std::fflush(stdout);
    ff_bench::export_samples(samples, origin, capacity, mode, p.name, repeat);
    return {monitor_end-monitor_start,cpu_end-cpu_start,process_end-process_start};
}
static void print_csv_header(const char* prefix = "", const char* suffix = "") {
    std::printf("%scapacity,mode,profile,repeat,samples,work,producer_cpu,consumer_cpu,completed_per_s,e2e_p50_us,e2e_p95_us,e2e_p99_us,e2e_p999_us,e2e_max_us,actual_p99_us,submit_lag_p50_us,submit_lag_p99_us,dispatch_p50_us,dispatch_p99_us,dispatch_max_us,service_p50_us,peak_outstanding,late_events,drain_last_submit_ms,drain_schedule_ms,sample_storage_mib%s\n",prefix,suffix);
}
}
#endif
