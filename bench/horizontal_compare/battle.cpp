// Success-only, bounded-concurrency comparisons. See battle README methodology.
#define ASIO_STANDALONE 1
#include <asio.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/use_awaitable.hpp>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "bench/load_probe.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#include "executor/uv_executor.h"
#include <ctime>
#include <limits>

namespace battle {
using namespace flux_foundry;
using load_probe::Ns;
using load_probe::now_ns;
using load_probe::relax;
using load_probe::require;
thread_local bool on_owner = false;
static_assert(__cplusplus >= 202002L, "battle builds every lane as C++20, including Asio coroutines");
static_assert(load_probe::Clock::is_steady, "latency measurements require a monotonic clock");
static_assert(std::atomic<std::size_t>::is_always_lock_free, "completion credits require lock-free size_t");

struct Backend {
    const bool native_uv;
    asio::io_context io{1}; // One run thread; concurrent producer post remains supported.
    asio::executor_work_guard<asio::io_context::executor_type> guard{io.get_executor()};
    uv_loop_t loop{};
    std::unique_ptr<uv_executor> uv;
    bool stopped = false; // Owner only.
    std::uint64_t activity = 0; // Owner only, counts benchmark envelope/work callbacks.
    explicit Backend(bool use_uv) : native_uv(use_uv) {}
    template<class F> void post(F&& fn) {
        if (native_uv) {
            task_wrapper_sbo task(std::forward<F>(fn));
            require(uv->try_dispatch(std::move(task)), "UV rejected work within the common credit bound");
        } else asio::post(io, std::forward<F>(fn));
    }
    void post_erased(task_wrapper_sbo task) {
        if (native_uv) require(uv->try_dispatch(std::move(task)), "UV rejected erased work");
        else asio::post(io, [task = std::move(task)]() mutable noexcept { task(); });
    }
    bool turn(bool block) {
        if (native_uv) return uv_run(&loop, block ? UV_RUN_ONCE : UV_RUN_NOWAIT) != 0;
        if (block) io.run_one();
        else for (unsigned i = 0; i < 16 && io.poll_one(); ++i) {}
        return !stopped;
    }
    void run(const char* policy) {
        if (!std::strcmp(policy, "blocking")) {
            if (native_uv) uv_run(&loop, UV_RUN_DEFAULT); else io.run();
            return;
        }
        const bool hybrid = !std::strcmp(policy, "hybrid-2000");
#if !defined(__linux__)
        require(!hybrid, "hybrid is a Linux-only candidate in the platform battle matrix");
#endif
        if (!hybrid) { while (turn(false)) relax(); return; }
        auto last = now_ns(); auto previous = activity; bool block = false;
        while (turn(block)) {
            const auto now = now_ns();
            if (activity != previous) last = now;
            previous = activity;
            block = now - last >= 2000;
            if (!block) relax();
        }
    }
    void close() {
        if (native_uv) uv->close();
        else asio::post(io, [this] { stopped = true; guard.reset(); });
    }
};

struct Sample {
    Ns planned = 0, submitted = 0, admitted = 0, begin = 0, end = 0;
    std::uint64_t value = 0;
    unsigned steps = 0, envelopes = 0, completions = 0;
};
struct Context {
    Backend* backend;
    std::vector<Sample>* samples;
    unsigned work;
    std::atomic<std::size_t> completed{0};
};
struct Msg { Context* ctx; std::size_t id; std::uint64_t value; unsigned hop = 0; };
using Result = result_t<Msg, std::exception_ptr>;
static_assert(std::is_nothrow_copy_constructible<Msg>::value && std::is_nothrow_move_constructible<Msg>::value,
              "identical payload movement must not introduce exceptions");
std::uint64_t transform(std::uint64_t value, unsigned work) noexcept {
    value += UINT64_C(0x9e3779b97f4a7c15);
    for (unsigned i = 0; i < work; ++i) {
        value ^= value >> 17; value *= UINT64_C(0xd6e8feb86659fd93); value ^= value << 11;
    }
    return value;
}
void envelope(Msg e) {
    require(on_owner, "envelope executed on producer");
    ++(*e.ctx->samples)[e.id].envelopes; ++e.ctx->backend->activity;
}
Msg step(Msg e) noexcept {
    require(on_owner, "work executed on producer");
    auto& sample = (*e.ctx->samples)[e.id];
    require(sample.steps == e.hop, "step order/duplication mismatch");
    if (!e.hop) sample.begin = now_ns();
    e.value = transform(e.value, e.ctx->work); ++e.hop;
    ++sample.steps; ++e.ctx->backend->activity;
    return e;
}
void finish(Msg e) noexcept {
    require(on_owner, "completion executed on producer");
    auto& sample = (*e.ctx->samples)[e.id];
    sample.value = e.value; ++sample.completions; sample.end = now_ns();
    e.ctx->completed.fetch_add(1, std::memory_order_release);
}
struct Receiver {
    using value_type = Result;
    void emplace(Result&& r) noexcept { require(r.has_value(), "FF flow failed"); finish(r.value()); }
};
struct Normal final : awaitable_base<Normal, Msg, std::exception_ptr> {
    using async_result_type = Result;
    Msg input;
    explicit Normal(Result&& r) noexcept : input(r.value()) {}
    bool available() const noexcept { return true; }
    int submit() noexcept {
        retain(); auto* self = this;
        input.ctx->backend->post_erased(task_wrapper_sbo([self]() noexcept {
            auto value = step(self->input);
            self->resume(Result(value_tag, value)); self->release();
        }));
        return 0;
    }
    void cancel() noexcept {}
};
struct Fast final : fast_awaitable_base<Fast, Msg, std::exception_ptr> {
    using async_result_type = Result;
    Msg input;
    explicit Fast(Result&& r) noexcept : input(r.value()) {}
    bool available() const noexcept { return true; }
    int submit() noexcept {
        auto* self = this;
        input.ctx->backend->post_erased(task_wrapper_sbo([self]() noexcept {
            auto value = step(self->input);
            self->resume(Result(value_tag, value)); // Final access to self.
        }));
        return 0;
    }
    void cancel() noexcept {}
};
template<class Awaitable, unsigned Hops> auto blueprint() {
    static_assert(Hops == 1 || Hops == 4, "compile-time graphs support exactly one or four steps");
    auto first = make_blueprint<Msg, std::exception_ptr>() | await<Awaitable>();
    if constexpr (Hops == 1) return std::move(first) | end();
    else return std::move(first) | await<Awaitable>() | await<Awaitable>() | await<Awaitable>() | end();
}
template<unsigned Hops, bool Erased> void callback_step(Msg e) {
    auto fn = [e]() mutable noexcept {
        e = step(e);
        if (e.hop == Hops) finish(e); else callback_step<Hops, Erased>(e);
    };
    if constexpr (Erased) e.ctx->backend->post_erased(task_wrapper_sbo(std::move(fn)));
    else e.ctx->backend->post(std::move(fn));
}
template<class Token> auto async_step(Msg e, Token&& token) {
    return asio::async_initiate<Token, void(Msg)>([e](auto handler) mutable {
        e.ctx->backend->post([e, handler = std::move(handler)]() mutable noexcept { handler(step(e)); });
    }, token);
}
template<unsigned Hops> asio::awaitable<Msg> coroutine(Msg e) {
    envelope(e);
    for (unsigned i = 0; i < Hops; ++i) e = co_await async_step(e, asio::use_awaitable);
    co_return e;
}
Ns cpu_time() {
    const auto ticks = std::clock(); require(ticks != std::clock_t(-1), "CPU clock unavailable");
    return static_cast<Ns>(static_cast<long double>(ticks) * 1000000000 / CLOCKS_PER_SEC);
}

template<unsigned Hops>
void measure(Backend& backend, const char* group, const char* lane, const char* profile,
             const char* policy, std::size_t count, unsigned work, std::size_t capacity,
             int producer_cpu, int owner_cpu, bool warmup) {
    std::vector<Sample> samples(count);
    Context ctx{&backend, &samples, work};
    auto normal_bp = blueprint<Normal, Hops>();
    auto normal_owner = make_lite_ptr<decltype(normal_bp)>(std::move(normal_bp));
    auto fast_bp = blueprint<Fast, Hops>();
    const bool chain = !std::strcmp(group, "chain");
    const bool saturated = !std::strcmp(profile, "saturated");
    enum class Lane { raw, erased, coroutine, normal, fast_normal, fast_fast };
    const auto selected = !std::strcmp(lane, "raw") ? Lane::raw :
        !std::strcmp(lane, "erased") ? Lane::erased :
        !std::strcmp(lane, "coroutine") ? Lane::coroutine :
        !std::strcmp(lane, "ff-normal") ? Lane::normal :
        !std::strcmp(lane, "ff-fast-normal") ? Lane::fast_normal : Lane::fast_fast;
    const auto launch_runner = [&](auto runner, Msg e) {
        if (chain) backend.post([runner = std::move(runner), e]() mutable noexcept { envelope(e); runner(e); });
        else runner(e);
    };
    const auto launch = [&](Msg e) {
        switch (selected) {
        case Lane::raw:
            if (chain) backend.post([e]() noexcept { envelope(e); callback_step<Hops, false>(e); });
            else callback_step<Hops, false>(e);
            break;
        case Lane::erased:
            if (chain) backend.post([e]() noexcept { envelope(e); callback_step<Hops, true>(e); });
            else callback_step<Hops, true>(e);
            break;
        case Lane::coroutine:
            asio::co_spawn(backend.io, coroutine<Hops>(e), [](std::exception_ptr error, Msg out) {
                require(!error, "Asio coroutine failed"); finish(out);
            });
            break;
        case Lane::normal: launch_runner(make_runner(normal_owner, Receiver{}), e); break;
        case Lane::fast_normal: launch_runner(make_fast_runner(normal_owner, Receiver{}), e); break;
        case Lane::fast_fast: launch_runner(make_fast_runner_view(fast_bp, Receiver{}), e); break;
        }
    };
    std::size_t admission_retries = 0, peak = 0;
    const auto cpu_start = cpu_time(); const auto start = now_ns(); const auto origin = start + 2000000;
    while (now_ns() < origin) relax();
    for (std::size_t i = 0; i < count; ++i) {
        auto& sample = samples[i];
        sample.planned = saturated ? now_ns() : origin + static_cast<Ns>(i) * 2000;
        while (now_ns() < sample.planned) relax();
        sample.submitted = now_ns();
        for (backoff_strategy<> backoff; i - ctx.completed.load(std::memory_order_acquire) >= capacity; backoff.yield())
            ++admission_retries;
        peak = std::max(peak, i + 1 - ctx.completed.load(std::memory_order_acquire));
        sample.admitted = now_ns();
        launch(Msg{&ctx, i, static_cast<std::uint64_t>(i), 0});
    }
    const auto timeout = now_ns() + INT64_C(30000000000);
    while (ctx.completed.load(std::memory_order_acquire) != count) {
        require(now_ns() < timeout, "completion timeout"); relax();
    }
    std::atomic<bool> fenced{false};
    backend.post([&]() noexcept { fenced.store(true, std::memory_order_release); });
    while (!fenced.load(std::memory_order_acquire)) { require(now_ns() < timeout, "fence timeout"); relax(); }
    const auto stop = now_ns(); const auto cpu_stop = cpu_time();
    std::vector<Ns> e2e, actual, admitted, lag, admission;
    Ns last_end = origin;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& s = samples[i];
        auto expected = static_cast<std::uint64_t>(i);
        for (unsigned j = 0; j < Hops; ++j) expected = transform(expected, work);
        require(s.steps == Hops && s.envelopes == unsigned(chain) && s.completions == 1 && s.value == expected,
                "work/hop/envelope/completion checksum mismatch");
        require(s.end >= s.begin && s.begin >= s.admitted && s.admitted >= s.submitted && s.submitted >= s.planned,
                "timestamp ordering invalid");
        e2e.push_back(s.end - s.planned); actual.push_back(s.end - s.submitted);
        admitted.push_back(s.end - s.admitted); lag.push_back(s.submitted - s.planned);
        admission.push_back(s.admitted - s.submitted); last_end = std::max(last_end, s.end);
    }
    const auto qe = load_probe::quantiles(e2e), qa = load_probe::quantiles(actual),
               qd = load_probe::quantiles(admitted), ql = load_probe::quantiles(lag), qi = load_probe::quantiles(admission);
    require(peak <= capacity && last_end > origin, "credit bound/timing invalid");
    std::printf("%s,%s,%s,%s,%s,%s,%u,%zu,%u,%zu,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%zu,%zu\n",
        warmup ? "warmup" : "measured", group, backend.native_uv ? "uv" : "asio", lane, profile, policy,
        Hops, count, work, capacity, producer_cpu, owner_cpu,
        count * 1e9 / (last_end - origin), qe.p50/1e3, qe.p99/1e3, qe.p999/1e3, qe.max/1e3,
        qa.p99/1e3, qd.p99/1e3, ql.p99/1e3, qi.p99/1e3,
        100.0 * (cpu_stop-cpu_start)/(stop-start), peak, admission_retries);
}
} // namespace battle

int main(int argc, char** argv) {
    using namespace battle;
    require(argc == 12, "usage: battle chain|handoff asio|uv LANE HOPS saturated|steady500k POLICY SAMPLES WORK CAPACITY OWNER_CPU PRODUCER_CPU");
    const char* group=argv[1]; const char* backend_name=argv[2]; const char* lane=argv[3];
    const auto hops=load_probe::number(argv[4]); const char* profile=argv[5]; const char* policy=argv[6];
    const auto samples=load_probe::number(argv[7]); const auto work=load_probe::number(argv[8]);
    const auto capacity=load_probe::number(argv[9]);
    const int owner_cpu=!std::strcmp(argv[10],"-1")?-1:static_cast<int>(load_probe::number(argv[10]));
    const int producer_cpu=!std::strcmp(argv[11],"-1")?-1:static_cast<int>(load_probe::number(argv[11]));
    require(!std::strcmp(group,"chain") || !std::strcmp(group,"handoff"), "invalid group");
    require(!std::strcmp(backend_name,"asio") || !std::strcmp(backend_name,"uv"), "invalid backend");
    require(!std::strcmp(lane,"raw") || !std::strcmp(lane,"erased") || !std::strcmp(lane,"coroutine") ||
            !std::strcmp(lane,"ff-normal") || !std::strcmp(lane,"ff-fast-normal") || !std::strcmp(lane,"ff-fast-fast"), "invalid lane");
    require(hops == 1 || hops == 4, "hops must be 1 or 4");
    require(!std::strcmp(profile,"saturated") || !std::strcmp(profile,"steady500k"), "invalid profile");
    require(!std::strcmp(policy,"blocking") || !std::strcmp(policy,"poll") || !std::strcmp(policy,"hybrid-2000"), "invalid policy");
    require(samples > 0 && samples <= 1000000 && work <= 1000000 && capacity > 0 && capacity <= 1024,
            "sample/work/capacity bounds invalid");
    require(owner_cpu < 0 || producer_cpu < 0 || owner_cpu != producer_cpu, "choose distinct CPUs");
    require(std::strcmp(group,"chain") || !std::strcmp(backend_name,"asio"), "chain uses a common Asio post backend");
    require(std::strcmp(group,"handoff") || (hops == 1 && std::strcmp(lane,"coroutine")), "handoff is one step; coroutine belongs to chain group");
    load_probe::pin(producer_cpu);
    Backend backend(!std::strcmp(backend_name,"uv")); std::atomic<bool> ready{false};
    std::thread owner([&] {
        load_probe::pin(owner_cpu); on_owner = true;
        if (backend.native_uv) {
            require(uv_loop_init(&backend.loop) == 0, "uv_loop_init failed");
            backend.uv.reset(new uv_executor(&backend.loop, 1024, 16));
        }
        ready.store(true, std::memory_order_release); backend.run(policy);
        if (backend.native_uv) {
            require(backend.uv->closed(), "UV close callback did not run");
            require(uv_loop_close(&backend.loop) == 0, "UV loop did not drain");
        }
    });
    while (!ready.load(std::memory_order_acquire)) relax();
    std::puts("phase,group,backend,lane,profile,policy,hops,samples,work,capacity,producer_cpu,owner_cpu,completed_per_s,e2e_p50_us,e2e_p99_us,e2e_p999_us,e2e_max_us,actual_p99_us,admitted_p99_us,submit_lag_p99_us,admission_wait_p99_us,cpu_percent,peak_inflight,admission_retries");
    for (bool warmup : {true,false}) {
        const auto count=warmup?std::min<std::size_t>(samples,4096):samples;
        if (hops == 1) measure<1>(backend,group,lane,profile,policy,count,work,capacity,producer_cpu,owner_cpu,warmup);
        else measure<4>(backend,group,lane,profile,policy,count,work,capacity,producer_cpu,owner_cpu,warmup);
    }
    backend.close(); owner.join();
    require(!backend.native_uv ? backend.io.stopped() : backend.uv->closed(), "backend failed to stop cleanly");
}
