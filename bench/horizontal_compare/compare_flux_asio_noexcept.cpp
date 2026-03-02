#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <system_error>
#include <thread>
#include <utility>

#define ASIO_STANDALONE 1
#define ASIO_NO_EXCEPTIONS 1
#define ASIO_DISABLE_EXCEPTIONS 1
#include <asio.hpp>

namespace asio {
namespace detail {
template <typename Exception>
[[noreturn]] void throw_exception(const Exception& ASIO_SOURCE_LOCATION_PARAM) {
    std::abort();
}
}
}

#include "base/traits.h"
#include "flow/flow_def.h"

namespace flux_foundry {
template <>
struct cancel_error<std::error_code> {
    static std::error_code make(cancel_kind kind) noexcept {
        return std::error_code(kind == cancel_kind::hard ? 1001 : 1002, std::generic_category());
    }
};

template <>
struct awaitable_creating_error<std::error_code> {
    static std::error_code make() noexcept {
        return std::error_code(1003, std::generic_category());
    }
};

template <>
struct async_submission_failed_error<std::error_code> {
    static std::error_code make() noexcept {
        return std::error_code(1004, std::generic_category());
    }
};

template <>
struct async_all_failed_error<std::error_code> {
    static std::error_code make() noexcept {
        return std::error_code(1005, std::generic_category());
    }
};

template <>
struct async_any_failed_error<std::error_code> {
    static std::error_code make(size_t i) noexcept {
        return std::error_code(static_cast<int>(1100 + i), std::generic_category());
    }
};
}

#include "flow/flow.h"
#include "executor/simple_executor.h"

using namespace flux_foundry;

namespace {

using err_t = std::error_code;
using out_t = result_t<int, err_t>;

struct inline_executor {
    void dispatch(task_wrapper_sbo t) noexcept {
        t();
    }
};

struct asio_dispatch_adapter {
    asio::io_context* io;
    void dispatch(task_wrapper_sbo t) noexcept {
        asio::dispatch(*io, [task = std::move(t)]() mutable noexcept {
            task();
        });
    }
};

struct asio_post_adapter {
    asio::io_context* io;
    void dispatch(task_wrapper_sbo t) noexcept {
        asio::post(*io, [task = std::move(t)]() mutable noexcept {
            task();
        });
    }
};

struct simple_executor_env {
    simple_executor<4096> ex;
    std::thread worker;

    simple_executor_env()
        : worker([this]() noexcept { ex.run(); }) {
        std::atomic<int> started{0};
        ex.dispatch(task_wrapper_sbo([&started]() noexcept {
            started.store(1, std::memory_order_release);
        }));
        while (started.load(std::memory_order_acquire) == 0) {
            std::this_thread::yield();
        }
    }

    ~simple_executor_env() {
        while (!ex.try_shutdown()) {
            std::this_thread::yield();
        }
        if (worker.joinable()) {
            worker.join();
        }
    }
};

struct simple_dispatch_adapter {
    simple_executor<4096>* ex;
    void dispatch(task_wrapper_sbo t) noexcept {
        ex->dispatch(std::move(t));
    }
};

struct immediate_plus_one_awaitable final : awaitable_base<immediate_plus_one_awaitable, int, err_t> {
    using async_result_type = out_t;

    int v;

    explicit immediate_plus_one_awaitable(async_result_type&& in) noexcept
        : v(in.has_value() ? in.value() : 0) {
    }

    int submit() noexcept {
        this->resume(async_result_type(value_tag, v + 1));
        return 0;
    }

    bool available() noexcept {
        return true;
    }

    void cancel() noexcept {
    }
};

struct immediate_plus_one_fast_awaitable final
    : fast_awaitable_base<immediate_plus_one_fast_awaitable, int, err_t> {
    using async_result_type = out_t;

    int v;

    explicit immediate_plus_one_fast_awaitable(async_result_type&& in) noexcept
        : v(in.has_value() ? in.value() : 0) {
    }

    int submit() noexcept {
        this->resume(async_result_type(value_tag, v + 1));
        return 0;
    }

    bool available() const noexcept {
        return true;
    }

    void cancel() noexcept {
    }
};

asio::thread_pool* g_real_backend_pool = nullptr;

struct backend_plus_one_fast_awaitable final
    : fast_awaitable_base<backend_plus_one_fast_awaitable, int, err_t> {
    using async_result_type = out_t;

    int v;

    explicit backend_plus_one_fast_awaitable(async_result_type&& in) noexcept
        : v(in.has_value() ? in.value() : 0) {
    }

    int submit() noexcept {
        auto* self = this;
        const int out = v + 1;
        asio::post(*g_real_backend_pool, [self, out]() noexcept {
            self->resume(async_result_type(value_tag, out));
        });
        return 0;
    }

    bool available() const noexcept {
        return g_real_backend_pool != nullptr;
    }

    void cancel() noexcept {
    }
};

struct sink_receiver {
    using value_type = out_t;
    volatile long long* sink;

    void emplace(value_type&& r) noexcept {
        if (r.has_value()) {
            *sink += static_cast<long long>(r.value());
        } else {
            *sink -= 1;
        }
    }
};

struct sync_wait_state {
    std::atomic<int> done{0};
};

struct sync_wait_receiver {
    using value_type = out_t;
    volatile long long* sink;
    sync_wait_state* state;

    void emplace(value_type&& r) noexcept {
        if (r.has_value()) {
            *sink += static_cast<long long>(r.value());
        } else {
            *sink -= 1;
        }
        state->done.store(1, std::memory_order_release);
    }
};

struct bench_result {
    const char* name;
    int warmup;
    int iters;
    long long elapsed_ns;
    double ns_per_op;
};

template <typename F>
bench_result run_bench(const char* name, int warmup, int iters, F&& fn) {
    for (int i = 0; i < warmup; ++i) {
        fn(i);
    }

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        fn(i);
    }
    auto t1 = std::chrono::steady_clock::now();

    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    return bench_result{name, warmup, iters, ns, static_cast<double>(ns) / static_cast<double>(iters)};
}

void print_result(const bench_result& r) {
    std::printf("%-28s warmup=%-7d iter=%-8d total=%.3f ms  ns/op=%.2f\n",
        r.name,
        r.warmup,
        r.iters,
        static_cast<double>(r.elapsed_ns) / 1e6,
        r.ns_per_op);
}

auto make_sync_20_bp() {
    auto bp = make_blueprint<int, err_t>()
        | transform([](int x) noexcept { return (x ^ 0x5a5a5a5a) + (x >> 3); })
        | transform([](int x) noexcept { return (x ^ 0x5a5a5a5a) + (x >> 3); })
        | transform([](int x) noexcept { return (x ^ 0x5a5a5a5a) + (x >> 3); })
        | transform([](int x) noexcept { return (x ^ 0x5a5a5a5a) + (x >> 3); })
        | transform([](int x) noexcept { return (x ^ 0x5a5a5a5a) + (x >> 3); })
        | transform([](int x) noexcept { return (x ^ 0x5a5a5a5a) + (x >> 3); })
        | transform([](int x) noexcept { return (x ^ 0x5a5a5a5a) + (x >> 3); })
        | transform([](int x) noexcept { return (x ^ 0x5a5a5a5a) + (x >> 3); })
        | transform([](int x) noexcept { return (x ^ 0x5a5a5a5a) + (x >> 3); })
        | transform([](int x) noexcept { return (x ^ 0x5a5a5a5a) + (x >> 3); })
        | transform([](int x) noexcept { return (x ^ 0x5a5a5a5a) + (x >> 3); })
        | transform([](int x) noexcept { return (x ^ 0x5a5a5a5a) + (x >> 3); })
        | transform([](int x) noexcept { return (x ^ 0x5a5a5a5a) + (x >> 3); })
        | transform([](int x) noexcept { return (x ^ 0x5a5a5a5a) + (x >> 3); })
        | transform([](int x) noexcept { return (x ^ 0x5a5a5a5a) + (x >> 3); })
        | transform([](int x) noexcept { return (x ^ 0x5a5a5a5a) + (x >> 3); })
        | transform([](int x) noexcept { return (x ^ 0x5a5a5a5a) + (x >> 3); })
        | transform([](int x) noexcept { return (x ^ 0x5a5a5a5a) + (x >> 3); })
        | transform([](int x) noexcept { return (x ^ 0x5a5a5a5a) + (x >> 3); })
        | transform([](int x) noexcept { return (x ^ 0x5a5a5a5a) + (x >> 3); })
        | end();
    return bp;
}

template <typename ExecutorPtr>
auto make_via_20_bp(ExecutorPtr ex) {
    auto bp = make_blueprint<int, err_t>()
        | via(ex) | via(ex) | via(ex) | via(ex) | via(ex)
        | via(ex) | via(ex) | via(ex) | via(ex) | via(ex)
        | via(ex) | via(ex) | via(ex) | via(ex) | via(ex)
        | via(ex) | via(ex) | via(ex) | via(ex) | via(ex)
        | end();
    return bp;
}

template <typename ExecutorPtr>
auto make_async_1_bp(ExecutorPtr ex) {
    auto bp = make_blueprint<int, err_t>()
        | await<immediate_plus_one_awaitable>(ex)
        | end();
    return bp;
}

template <typename ExecutorPtr>
auto make_async_1_fast_bp(ExecutorPtr ex) {
    auto bp = make_blueprint<int, err_t>()
        | await<immediate_plus_one_fast_awaitable>(ex)
        | end();
    return bp;
}

auto make_async_1_bp_inline() {
    auto bp = make_blueprint<int, err_t>()
        | await<immediate_plus_one_awaitable>()
        | end();
    return bp;
}

auto make_async_1_fast_bp_inline() {
    auto bp = make_blueprint<int, err_t>()
        | await<immediate_plus_one_fast_awaitable>()
        | end();
    return bp;
}

template <typename ExecutorPtr>
auto make_async_4_bp(ExecutorPtr ex) {
    auto bp = make_blueprint<int, err_t>()
        | await<immediate_plus_one_awaitable>(ex)
        | await<immediate_plus_one_awaitable>(ex)
        | await<immediate_plus_one_awaitable>(ex)
        | await<immediate_plus_one_awaitable>(ex)
        | end();
    return bp;
}

template <typename ExecutorPtr>
auto make_async_4_fast_bp(ExecutorPtr ex) {
    auto bp = make_blueprint<int, err_t>()
        | await<immediate_plus_one_fast_awaitable>(ex)
        | await<immediate_plus_one_fast_awaitable>(ex)
        | await<immediate_plus_one_fast_awaitable>(ex)
        | await<immediate_plus_one_fast_awaitable>(ex)
        | end();
    return bp;
}

auto make_async_4_bp_inline() {
    auto bp = make_blueprint<int, err_t>()
        | await<immediate_plus_one_awaitable>()
        | await<immediate_plus_one_awaitable>()
        | await<immediate_plus_one_awaitable>()
        | await<immediate_plus_one_awaitable>()
        | end();
    return bp;
}

auto make_async_4_fast_bp_inline() {
    auto bp = make_blueprint<int, err_t>()
        | await<immediate_plus_one_fast_awaitable>()
        | await<immediate_plus_one_fast_awaitable>()
        | await<immediate_plus_one_fast_awaitable>()
        | await<immediate_plus_one_fast_awaitable>()
        | end();
    return bp;
}

auto make_async_1_real_backend_fast_bp() {
    auto bp = make_blueprint<int, err_t>()
        | await<backend_plus_one_fast_awaitable>()
        | end();
    return bp;
}

auto make_async_4_real_backend_fast_bp() {
    auto bp = make_blueprint<int, err_t>()
        | await<backend_plus_one_fast_awaitable>()
        | await<backend_plus_one_fast_awaitable>()
        | await<backend_plus_one_fast_awaitable>()
        | await<backend_plus_one_fast_awaitable>()
        | end();
    return bp;
}

inline void drain(asio::io_context& io) {
    io.restart();
    io.run();
}

inline void drain(simple_executor_env& env) {
    std::atomic<int> done{0};
    env.ex.dispatch(task_wrapper_sbo([&done]() noexcept {
        done.store(1, std::memory_order_release);
    }));
    while (done.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }
}

struct asio_dispatch20_hop {
    asio::io_context* io;
    volatile long long* sink;
    int x;
    int step;

    void operator()() const {
        const int nx = x + 1;
        const int ns = step + 1;
        if (ns < 20) {
            asio::dispatch(*io, asio_dispatch20_hop{io, sink, nx, ns});
            return;
        }
        *sink += static_cast<long long>(nx);
    }
};

void asio_dispatch_sync20(asio::io_context& io, int in, volatile long long* sink) {
    asio::dispatch(io, asio_dispatch20_hop{&io, sink, in, 0});
    drain(io);
}

struct asio_post20_hop {
    asio::io_context* io;
    volatile long long* sink;
    int x;
    int step;

    void operator()() const {
        const int nx = x + 1;
        const int ns = step + 1;
        if (ns < 20) {
            asio::post(*io, asio_post20_hop{io, sink, nx, ns});
            return;
        }
        *sink += static_cast<long long>(nx);
    }
};

void asio_post_sync20(asio::io_context& io, int in, volatile long long* sink) {
    asio::post(io, asio_post20_hop{&io, sink, in, 0});
    drain(io);
}

struct asio_post_async4_hop {
    asio::io_context* io;
    volatile long long* sink;
    int x;
    int remaining;

    void operator()() const {
        const int nx = x + 1;
        const int nr = remaining - 1;
        if (nr > 0) {
            asio::post(*io, asio_post_async4_hop{io, sink, nx, nr});
            return;
        }
        *sink += static_cast<long long>(nx);
    }
};

struct asio_dispatch_async4_hop {
    asio::io_context* io;
    volatile long long* sink;
    int x;
    int remaining;

    void operator()() const {
        const int nx = x + 1;
        const int nr = remaining - 1;
        if (nr > 0) {
            asio::dispatch(*io, asio_dispatch_async4_hop{io, sink, nx, nr});
            return;
        }
        *sink += static_cast<long long>(nx);
    }
};

struct asio_post_adapter_async_hop {
    asio_post_adapter* ex;
    volatile long long* sink;
    int x;
    int remaining;

    void operator()() const noexcept {
        const int nx = x + 1;
        const int nr = remaining - 1;
        if (nr > 0) {
            ex->dispatch(task_wrapper_sbo(asio_post_adapter_async_hop{ex, sink, nx, nr}));
            return;
        }
        *sink += static_cast<long long>(nx);
    }
};

struct asio_dispatch_adapter_async_hop {
    asio_dispatch_adapter* ex;
    volatile long long* sink;
    int x;
    int remaining;

    void operator()() const noexcept {
        const int nx = x + 1;
        const int nr = remaining - 1;
        if (nr > 0) {
            ex->dispatch(task_wrapper_sbo(asio_dispatch_adapter_async_hop{ex, sink, nx, nr}));
            return;
        }
        *sink += static_cast<long long>(nx);
    }
};

void asio_dispatch_async4(asio::io_context& io, int in, volatile long long* sink) {
    asio::dispatch(io, asio_dispatch_async4_hop{&io, sink, in, 4});
    drain(io);
}

void asio_dispatch_async1(asio::io_context& io, int in, volatile long long* sink) {
    asio::dispatch(io, asio_dispatch_async4_hop{&io, sink, in, 1});
    drain(io);
}

void asio_post_async4(asio::io_context& io, int in, volatile long long* sink) {
    asio::post(io, asio_post_async4_hop{&io, sink, in, 4});
    drain(io);
}

void asio_post_async1(asio::io_context& io, int in, volatile long long* sink) {
    asio::post(io, asio_post_async4_hop{&io, sink, in, 1});
    drain(io);
}

void asio_dispatch_adapter_async4(asio_dispatch_adapter& ex, asio::io_context& io, int in, volatile long long* sink) {
    ex.dispatch(task_wrapper_sbo(asio_dispatch_adapter_async_hop{&ex, sink, in, 4}));
    drain(io);
}

void asio_dispatch_adapter_async1(asio_dispatch_adapter& ex, asio::io_context& io, int in, volatile long long* sink) {
    ex.dispatch(task_wrapper_sbo(asio_dispatch_adapter_async_hop{&ex, sink, in, 1}));
    drain(io);
}

void asio_post_adapter_async4(asio_post_adapter& ex, asio::io_context& io, int in, volatile long long* sink) {
    ex.dispatch(task_wrapper_sbo(asio_post_adapter_async_hop{&ex, sink, in, 4}));
    drain(io);
}

void asio_post_adapter_async1(asio_post_adapter& ex, asio::io_context& io, int in, volatile long long* sink) {
    ex.dispatch(task_wrapper_sbo(asio_post_adapter_async_hop{&ex, sink, in, 1}));
    drain(io);
}

void asio_post_when_all2(asio::io_context& io, int a_in, int b_in, volatile long long* sink) {
    int a = 0;
    int b = 0;
    int remaining = 2;
    auto done = [&]() {
        --remaining;
        if (remaining == 0) {
            *sink += static_cast<long long>(a + b);
        }
    };

    asio::post(io, [&]() {
        a = a_in + 10;
        done();
    });
    asio::post(io, [&]() {
        b = b_in + 20;
        done();
    });

    drain(io);
}

void asio_post_when_any2(asio::io_context& io, int a_in, int b_in, volatile long long* sink) {
    bool has = false;
    int out = 0;
    auto choose = [&](int v) {
        if (!has) {
            has = true;
            out = v;
        }
    };

    asio::post(io, [&]() { choose(a_in + 100); });
    asio::post(io, [&]() { choose(b_in + 200); });

    drain(io);
    *sink += static_cast<long long>(out);
}

struct asio_real_backend_async_hop {
    asio::thread_pool* pool;
    volatile long long* sink;
    std::atomic<int>* done;
    int x;
    int remaining;

    void operator()() const noexcept {
        const int nx = x + 1;
        const int nr = remaining - 1;
        if (nr > 0) {
            asio::post(*pool, asio_real_backend_async_hop{pool, sink, done, nx, nr});
            return;
        }
        *sink += static_cast<long long>(nx);
        done->store(1, std::memory_order_release);
    }
};

void asio_real_backend_async4(asio::thread_pool& pool, int in, volatile long long* sink, std::atomic<int>& done) noexcept {
    asio::post(pool, asio_real_backend_async_hop{&pool, sink, &done, in, 4});
}

void asio_real_backend_async1(asio::thread_pool& pool, int in, volatile long long* sink, std::atomic<int>& done) noexcept {
    asio::post(pool, asio_real_backend_async_hop{&pool, sink, &done, in, 1});
}

} // namespace

int main() {
    std::printf("[horizontal compare noexc] flux_foundry vs asio (same host/toolchain)\n");
    std::printf("[build] clang++ -std=c++14 -O3 -fno-exceptions -DFLUX_FOUNDRY_NO_EXCEPTION_STRICT=1 -DASIO_NO_EXCEPTIONS=1\n");

    volatile long long sink = 0;
    asio::io_context io_sched_dispatch;
    asio::io_context io_sched_post;
    asio::io_context io_full_flux_dispatch;
    asio::io_context io_full_flux;
    asio::io_context io_full_asio_dispatch;
    asio::io_context io_full_asio;
    simple_executor_env flux_native_exec_env;

    asio_dispatch_adapter ex_sched_dispatch{&io_sched_dispatch};
    asio_post_adapter ex_sched_post{&io_sched_post};
    asio_dispatch_adapter ex_full_dispatch{&io_full_flux_dispatch};
    asio_post_adapter ex_full_post{&io_full_flux};
    asio_dispatch_adapter ex_full_asio_dispatch_adapter{&io_full_asio_dispatch};
    asio_post_adapter ex_full_asio_post_adapter{&io_full_asio};
    simple_dispatch_adapter ex_native_dispatch{&flux_native_exec_env.ex};
    inline_executor ex_native_inline;

    auto r0 = run_bench("baseline.direct.loop20", 10000, 3000000, [&](int i) {
        int x = i;
         x = (x ^ 0x5a5a5a5a) + (x >> 3);  x = (x ^ 0x5a5a5a5a) + (x >> 3);  x = (x ^ 0x5a5a5a5a) + (x >> 3);  x = (x ^ 0x5a5a5a5a) + (x >> 3);  x = (x ^ 0x5a5a5a5a) + (x >> 3);
         x = (x ^ 0x5a5a5a5a) + (x >> 3);  x = (x ^ 0x5a5a5a5a) + (x >> 3);  x = (x ^ 0x5a5a5a5a) + (x >> 3);  x = (x ^ 0x5a5a5a5a) + (x >> 3);  x = (x ^ 0x5a5a5a5a) + (x >> 3);
         x = (x ^ 0x5a5a5a5a) + (x >> 3);  x = (x ^ 0x5a5a5a5a) + (x >> 3);  x = (x ^ 0x5a5a5a5a) + (x >> 3);  x = (x ^ 0x5a5a5a5a) + (x >> 3);  x = (x ^ 0x5a5a5a5a) + (x >> 3);
         x = (x ^ 0x5a5a5a5a) + (x >> 3);  x = (x ^ 0x5a5a5a5a) + (x >> 3);  x = (x ^ 0x5a5a5a5a) + (x >> 3);  x = (x ^ 0x5a5a5a5a) + (x >> 3);  x = (x ^ 0x5a5a5a5a) + (x >> 3);
        sink += static_cast<long long>(x);
    });
    print_result(r0);

    std::printf("\n[pure scheduling overhead]\n");
    auto bp_via_inline = make_via_20_bp(&ex_native_inline);
    auto bp_via_inline_ptr = make_lite_ptr<decltype(bp_via_inline)>(std::move(bp_via_inline));
    auto flux_via_inline_runner = make_runner(bp_via_inline_ptr, sink_receiver{&sink});
    auto r0a = run_bench("sched.flux.native.via20.inline", 5000, 300000, [&](int i) {
        flux_via_inline_runner(i);
    });
    print_result(r0a);

    auto bp_via_native_dispatch = make_via_20_bp(&ex_native_dispatch);
    auto bp_via_native_dispatch_ptr = make_lite_ptr<decltype(bp_via_native_dispatch)>(std::move(bp_via_native_dispatch));
    auto flux_via_native_dispatch_runner = make_runner(bp_via_native_dispatch_ptr, sink_receiver{&sink});
    auto r0b = run_bench("sched.flux.native.via20.dispatch", 5000, 200000, [&](int i) {
        flux_via_native_dispatch_runner(i);
        drain(flux_native_exec_env);
    });
    print_result(r0b);

    auto bp_via_dispatch = make_via_20_bp(&ex_sched_dispatch);
    auto bp_via_dispatch_ptr = make_lite_ptr<decltype(bp_via_dispatch)>(std::move(bp_via_dispatch));
    auto flux_via_dispatch_runner = make_runner(bp_via_dispatch_ptr, sink_receiver{&sink});
    auto r1 = run_bench("sched.flux.on_asio.via20.dispatch", 5000, 200000, [&](int i) {
        flux_via_dispatch_runner(i);
        drain(io_sched_dispatch);
    });
    print_result(r1);

    auto r2 = run_bench("sched.asio.raw.dispatch20", 5000, 200000, [&](int i) {
        asio_dispatch_sync20(io_sched_dispatch, i, &sink);
    });
    print_result(r2);

    auto bp_via_post = make_via_20_bp(&ex_sched_post);
    auto bp_via_post_ptr = make_lite_ptr<decltype(bp_via_post)>(std::move(bp_via_post));
    auto flux_via_post_runner = make_runner(bp_via_post_ptr, sink_receiver{&sink});
    auto r3 = run_bench("sched.flux.on_asio.via20.post", 5000, 120000, [&](int i) {
        flux_via_post_runner(i);
        drain(io_sched_post);
    });
    print_result(r3);

    auto r4 = run_bench("sched.asio.raw.post20", 5000, 120000, [&](int i) {
        asio_post_sync20(io_sched_post, i, &sink);
    });
    print_result(r4);

    std::printf("\n[full semantics overhead]\n");
    auto bp_sync_std = make_sync_20_bp();
    auto bp_sync_std_ptr = make_lite_ptr<decltype(bp_sync_std)>(std::move(bp_sync_std));
    auto flux_sync_runner = make_runner(bp_sync_std_ptr, sink_receiver{&sink});
    auto r5 = run_bench("full.flux.native.runner.sync20", 10000, 800000, [&](int i) {
        flux_sync_runner(i);
    });
    print_result(r5);

    auto bp_sync_fast = make_sync_20_bp();
    auto flux_fast_runner = make_fast_runner(std::move(bp_sync_fast), sink_receiver{&sink});
    auto r6 = run_bench("full.flux.native.fast_runner.sync20", 10000, 1200000, [&](int i) {
        flux_fast_runner(i);
    });
    print_result(r6);

    auto bp_async4_native_inline = make_async_4_bp_inline();
    auto bp_async4_native_inline_ptr = make_lite_ptr<decltype(bp_async4_native_inline)>(std::move(bp_async4_native_inline));
    auto flux_async4_native_inline_runner = make_runner(bp_async4_native_inline_ptr, sink_receiver{&sink});
    auto r6n0 = run_bench("full.flux.native.runner.async4.inline", 5000, 300000, [&](int i) {
        flux_async4_native_inline_runner(i);
    });
    print_result(r6n0);

    auto bp_async4_native_inline_fast = make_async_4_bp_inline();
    auto flux_async4_native_inline_fast_runner = make_fast_runner_view(bp_async4_native_inline_fast, sink_receiver{&sink});
    auto r6n1 = run_bench("full.flux.native.fast_runner.async4.inline", 5000, 300000, [&](int i) {
        flux_async4_native_inline_fast_runner(i);
    });
    print_result(r6n1);

    auto bp_async4_native_inline_fastawait = make_async_4_fast_bp_inline();
    auto bp_async4_native_inline_fastawait_ptr = make_lite_ptr<decltype(bp_async4_native_inline_fastawait)>(std::move(bp_async4_native_inline_fastawait));
    auto flux_async4_native_inline_fastawait_runner = make_runner(bp_async4_native_inline_fastawait_ptr, sink_receiver{&sink});
    auto r6n1a = run_bench("full.flux.native.runner.fast_awaitable.async4.inline", 5000, 300000, [&](int i) {
        flux_async4_native_inline_fastawait_runner(i);
    });
    print_result(r6n1a);

    auto bp_async4_native_inline_fastawait_fast = make_async_4_fast_bp_inline();
    auto flux_async4_native_inline_fastawait_fast_runner =
        make_fast_runner_view(bp_async4_native_inline_fastawait_fast, sink_receiver{&sink});
    auto r6n1b = run_bench("full.flux.native.fast_runner.fast_awaitable.async4.inline", 5000, 300000, [&](int i) {
        flux_async4_native_inline_fastawait_fast_runner(i);
    });
    print_result(r6n1b);

    auto bp_async4_native_dispatch = make_async_4_bp(&ex_native_dispatch);
    auto bp_async4_native_dispatch_ptr = make_lite_ptr<decltype(bp_async4_native_dispatch)>(std::move(bp_async4_native_dispatch));
    auto flux_async4_native_dispatch_runner = make_runner(bp_async4_native_dispatch_ptr, sink_receiver{&sink});
    auto r6n2 = run_bench("full.flux.native.runner.async4.dispatch", 5000, 180000, [&](int i) {
        flux_async4_native_dispatch_runner(i);
        drain(flux_native_exec_env);
    });
    print_result(r6n2);

    auto bp_async4_native_dispatch_fast = make_async_4_bp(&ex_native_dispatch);
    auto flux_async4_native_dispatch_fast_runner = make_fast_runner_view(bp_async4_native_dispatch_fast, sink_receiver{&sink});
    auto r6n3 = run_bench("full.flux.native.fast_runner.async4.dispatch", 5000, 180000, [&](int i) {
        flux_async4_native_dispatch_fast_runner(i);
        drain(flux_native_exec_env);
    });
    print_result(r6n3);

    auto bp_async4_native_dispatch_fastawait = make_async_4_fast_bp(&ex_native_dispatch);
    auto bp_async4_native_dispatch_fastawait_ptr = make_lite_ptr<decltype(bp_async4_native_dispatch_fastawait)>(std::move(bp_async4_native_dispatch_fastawait));
    auto flux_async4_native_dispatch_fastawait_runner = make_runner(bp_async4_native_dispatch_fastawait_ptr, sink_receiver{&sink});
    auto r6n4 = run_bench("full.flux.native.runner.fast_awaitable.async4.dispatch", 5000, 180000, [&](int i) {
        flux_async4_native_dispatch_fastawait_runner(i);
        drain(flux_native_exec_env);
    });
    print_result(r6n4);

    auto bp_async4_native_dispatch_fastawait_fast = make_async_4_fast_bp(&ex_native_dispatch);
    auto flux_async4_native_dispatch_fastawait_fast_runner =
        make_fast_runner_view(bp_async4_native_dispatch_fastawait_fast, sink_receiver{&sink});
    auto r6n5 = run_bench("full.flux.native.fast_runner.fast_awaitable.async4.dispatch", 5000, 180000, [&](int i) {
        flux_async4_native_dispatch_fastawait_fast_runner(i);
        drain(flux_native_exec_env);
    });
    print_result(r6n5);

    auto bp_async_dispatch = make_async_4_bp(&ex_full_dispatch);
    auto bp_async_dispatch_ptr = make_lite_ptr<decltype(bp_async_dispatch)>(std::move(bp_async_dispatch));
    auto flux_async_dispatch_runner = make_runner(bp_async_dispatch_ptr, sink_receiver{&sink});
    auto r6a = run_bench("full.flux.on_asio.runner.async4.dispatch", 5000, 180000, [&](int i) {
        flux_async_dispatch_runner(i);
        drain(io_full_flux_dispatch);
    });
    print_result(r6a);

    auto bp_async_fast_dispatch = make_async_4_bp(&ex_full_dispatch);
    auto flux_async_fast_dispatch_runner = make_fast_runner_view(bp_async_fast_dispatch, sink_receiver{&sink});
    auto r6b = run_bench("full.flux.on_asio.fast_runner.async4.dispatch", 5000, 180000, [&](int i) {
        flux_async_fast_dispatch_runner(i);
        drain(io_full_flux_dispatch);
    });
    print_result(r6b);

    auto bp_async_fastawait_dispatch = make_async_4_fast_bp(&ex_full_dispatch);
    auto bp_async_fastawait_dispatch_ptr = make_lite_ptr<decltype(bp_async_fastawait_dispatch)>(std::move(bp_async_fastawait_dispatch));
    auto flux_async_fastawait_dispatch_runner = make_runner(bp_async_fastawait_dispatch_ptr, sink_receiver{&sink});
    auto r6d = run_bench("full.flux.on_asio.runner.fast_awaitable.async4.dispatch", 5000, 180000, [&](int i) {
        flux_async_fastawait_dispatch_runner(i);
        drain(io_full_flux_dispatch);
    });
    print_result(r6d);

    auto bp_async_fastawait_dispatch_fast = make_async_4_fast_bp(&ex_full_dispatch);
    auto flux_async_fastawait_dispatch_fast_runner =
        make_fast_runner_view(bp_async_fastawait_dispatch_fast, sink_receiver{&sink});
    auto r6e = run_bench("full.flux.on_asio.fast_runner.fast_awaitable.async4.dispatch", 5000, 180000, [&](int i) {
        flux_async_fastawait_dispatch_fast_runner(i);
        drain(io_full_flux_dispatch);
    });
    print_result(r6e);

    auto r6c = run_bench("full.asio.raw.dispatch.async4", 5000, 180000, [&](int i) {
        asio_dispatch_async4(io_full_asio_dispatch, i, &sink);
    });
    print_result(r6c);

    auto r6c2 = run_bench("full.asio.adapter.dispatch.async4", 5000, 180000, [&](int i) {
        asio_dispatch_adapter_async4(ex_full_asio_dispatch_adapter, io_full_asio_dispatch, i, &sink);
    });
    print_result(r6c2);

    auto bp_async = make_async_4_bp(&ex_full_post);
    auto bp_async_ptr = make_lite_ptr<decltype(bp_async)>(std::move(bp_async));
    auto flux_async_runner = make_runner(bp_async_ptr, sink_receiver{&sink});
    auto r7 = run_bench("full.flux.on_asio.runner.async4.post", 5000, 180000, [&](int i) {
        flux_async_runner(i);
        drain(io_full_flux);
    });
    print_result(r7);

    auto bp_async_fast = make_async_4_bp(&ex_full_post);
    auto flux_async_fast_runner = make_fast_runner_view(bp_async_fast, sink_receiver{&sink});
    auto r8 = run_bench("full.flux.on_asio.fast_runner.async4.post", 5000, 180000, [&](int i) {
        flux_async_fast_runner(i);
        drain(io_full_flux);
    });
    print_result(r8);

    auto bp_async_fastawait = make_async_4_fast_bp(&ex_full_post);
    auto bp_async_fastawait_ptr = make_lite_ptr<decltype(bp_async_fastawait)>(std::move(bp_async_fastawait));
    auto flux_async_fastawait_runner = make_runner(bp_async_fastawait_ptr, sink_receiver{&sink});
    auto r8a = run_bench("full.flux.on_asio.runner.fast_awaitable.async4.post", 5000, 180000, [&](int i) {
        flux_async_fastawait_runner(i);
        drain(io_full_flux);
    });
    print_result(r8a);

    auto bp_async_fastawait_fast = make_async_4_fast_bp(&ex_full_post);
    auto flux_async_fastawait_fast_runner = make_fast_runner_view(bp_async_fastawait_fast, sink_receiver{&sink});
    auto r8b = run_bench("full.flux.on_asio.fast_runner.fast_awaitable.async4.post", 5000, 180000, [&](int i) {
        flux_async_fastawait_fast_runner(i);
        drain(io_full_flux);
    });
    print_result(r8b);

    auto r9 = run_bench("full.asio.raw.post.async4", 5000, 180000, [&](int i) {
        asio_post_async4(io_full_asio, i, &sink);
    });
    print_result(r9);

    auto r9x = run_bench("full.asio.adapter.post.async4", 5000, 180000, [&](int i) {
        asio_post_adapter_async4(ex_full_asio_post_adapter, io_full_asio, i, &sink);
    });
    print_result(r9x);

    auto bp_async1_native_inline = make_async_1_bp_inline();
    auto bp_async1_native_inline_ptr = make_lite_ptr<decltype(bp_async1_native_inline)>(std::move(bp_async1_native_inline));
    auto flux_async1_native_inline_runner = make_runner(bp_async1_native_inline_ptr, sink_receiver{&sink});
    auto r9n0 = run_bench("full.flux.native.runner.async1.inline", 5000, 500000, [&](int i) {
        flux_async1_native_inline_runner(i);
    });
    print_result(r9n0);

    auto bp_async1_native_inline_fast = make_async_1_bp_inline();
    auto flux_async1_native_inline_fast_runner = make_fast_runner_view(bp_async1_native_inline_fast, sink_receiver{&sink});
    auto r9n1 = run_bench("full.flux.native.fast_runner.async1.inline", 5000, 500000, [&](int i) {
        flux_async1_native_inline_fast_runner(i);
    });
    print_result(r9n1);

    auto bp_async1_native_inline_fastawait = make_async_1_fast_bp_inline();
    auto bp_async1_native_inline_fastawait_ptr = make_lite_ptr<decltype(bp_async1_native_inline_fastawait)>(std::move(bp_async1_native_inline_fastawait));
    auto flux_async1_native_inline_fastawait_runner = make_runner(bp_async1_native_inline_fastawait_ptr, sink_receiver{&sink});
    auto r9n1a = run_bench("full.flux.native.runner.fast_awaitable.async1.inline", 5000, 500000, [&](int i) {
        flux_async1_native_inline_fastawait_runner(i);
    });
    print_result(r9n1a);

    auto bp_async1_native_inline_fastawait_fast = make_async_1_fast_bp_inline();
    auto flux_async1_native_inline_fastawait_fast_runner =
        make_fast_runner_view(bp_async1_native_inline_fastawait_fast, sink_receiver{&sink});
    auto r9n1b = run_bench("full.flux.native.fast_runner.fast_awaitable.async1.inline", 5000, 500000, [&](int i) {
        flux_async1_native_inline_fastawait_fast_runner(i);
    });
    print_result(r9n1b);

    auto bp_async1_native_dispatch = make_async_1_bp(&ex_native_dispatch);
    auto bp_async1_native_dispatch_ptr = make_lite_ptr<decltype(bp_async1_native_dispatch)>(std::move(bp_async1_native_dispatch));
    auto flux_async1_native_dispatch_runner = make_runner(bp_async1_native_dispatch_ptr, sink_receiver{&sink});
    auto r9n2 = run_bench("full.flux.native.runner.async1.dispatch", 5000, 300000, [&](int i) {
        flux_async1_native_dispatch_runner(i);
        drain(flux_native_exec_env);
    });
    print_result(r9n2);

    auto bp_async1_native_dispatch_fast = make_async_1_bp(&ex_native_dispatch);
    auto flux_async1_native_dispatch_fast_runner = make_fast_runner_view(bp_async1_native_dispatch_fast, sink_receiver{&sink});
    auto r9n3 = run_bench("full.flux.native.fast_runner.async1.dispatch", 5000, 300000, [&](int i) {
        flux_async1_native_dispatch_fast_runner(i);
        drain(flux_native_exec_env);
    });
    print_result(r9n3);

    auto bp_async1_native_dispatch_fastawait = make_async_1_fast_bp(&ex_native_dispatch);
    auto bp_async1_native_dispatch_fastawait_ptr = make_lite_ptr<decltype(bp_async1_native_dispatch_fastawait)>(std::move(bp_async1_native_dispatch_fastawait));
    auto flux_async1_native_dispatch_fastawait_runner = make_runner(bp_async1_native_dispatch_fastawait_ptr, sink_receiver{&sink});
    auto r9n4 = run_bench("full.flux.native.runner.fast_awaitable.async1.dispatch", 5000, 300000, [&](int i) {
        flux_async1_native_dispatch_fastawait_runner(i);
        drain(flux_native_exec_env);
    });
    print_result(r9n4);

    auto bp_async1_native_dispatch_fastawait_fast = make_async_1_fast_bp(&ex_native_dispatch);
    auto flux_async1_native_dispatch_fastawait_fast_runner =
        make_fast_runner_view(bp_async1_native_dispatch_fastawait_fast, sink_receiver{&sink});
    auto r9n5 = run_bench("full.flux.native.fast_runner.fast_awaitable.async1.dispatch", 5000, 300000, [&](int i) {
        flux_async1_native_dispatch_fastawait_fast_runner(i);
        drain(flux_native_exec_env);
    });
    print_result(r9n5);

    auto bp_async1_dispatch = make_async_1_bp(&ex_full_dispatch);
    auto bp_async1_dispatch_ptr = make_lite_ptr<decltype(bp_async1_dispatch)>(std::move(bp_async1_dispatch));
    auto flux_async1_dispatch_runner = make_runner(bp_async1_dispatch_ptr, sink_receiver{&sink});
    auto r9d = run_bench("full.flux.on_asio.runner.async1.dispatch", 5000, 300000, [&](int i) {
        flux_async1_dispatch_runner(i);
        drain(io_full_flux_dispatch);
    });
    print_result(r9d);

    auto bp_async1_fast_dispatch = make_async_1_bp(&ex_full_dispatch);
    auto flux_async1_fast_dispatch_runner = make_fast_runner_view(bp_async1_fast_dispatch, sink_receiver{&sink});
    auto r9e = run_bench("full.flux.on_asio.fast_runner.async1.dispatch", 5000, 300000, [&](int i) {
        flux_async1_fast_dispatch_runner(i);
        drain(io_full_flux_dispatch);
    });
    print_result(r9e);

    auto bp_async1_fastawait_dispatch = make_async_1_fast_bp(&ex_full_dispatch);
    auto bp_async1_fastawait_dispatch_ptr = make_lite_ptr<decltype(bp_async1_fastawait_dispatch)>(std::move(bp_async1_fastawait_dispatch));
    auto flux_async1_fastawait_dispatch_runner = make_runner(bp_async1_fastawait_dispatch_ptr, sink_receiver{&sink});
    auto r9g = run_bench("full.flux.on_asio.runner.fast_awaitable.async1.dispatch", 5000, 300000, [&](int i) {
        flux_async1_fastawait_dispatch_runner(i);
        drain(io_full_flux_dispatch);
    });
    print_result(r9g);

    auto bp_async1_fastawait_dispatch_fast = make_async_1_fast_bp(&ex_full_dispatch);
    auto flux_async1_fastawait_dispatch_fast_runner =
        make_fast_runner_view(bp_async1_fastawait_dispatch_fast, sink_receiver{&sink});
    auto r9h = run_bench("full.flux.on_asio.fast_runner.fast_awaitable.async1.dispatch", 5000, 300000, [&](int i) {
        flux_async1_fastawait_dispatch_fast_runner(i);
        drain(io_full_flux_dispatch);
    });
    print_result(r9h);

    auto r9f = run_bench("full.asio.raw.dispatch.async1", 5000, 300000, [&](int i) {
        asio_dispatch_async1(io_full_asio_dispatch, i, &sink);
    });
    print_result(r9f);

    auto r9f2 = run_bench("full.asio.adapter.dispatch.async1", 5000, 300000, [&](int i) {
        asio_dispatch_adapter_async1(ex_full_asio_dispatch_adapter, io_full_asio_dispatch, i, &sink);
    });
    print_result(r9f2);

    auto bp_async1 = make_async_1_bp(&ex_full_post);
    auto bp_async1_ptr = make_lite_ptr<decltype(bp_async1)>(std::move(bp_async1));
    auto flux_async1_runner = make_runner(bp_async1_ptr, sink_receiver{&sink});
    auto r9a = run_bench("full.flux.on_asio.runner.async1.post", 5000, 300000, [&](int i) {
        flux_async1_runner(i);
        drain(io_full_flux);
    });
    print_result(r9a);

    auto bp_async1_fast = make_async_1_bp(&ex_full_post);
    auto flux_async1_fast_runner = make_fast_runner_view(bp_async1_fast, sink_receiver{&sink});
    auto r9b = run_bench("full.flux.on_asio.fast_runner.async1.post", 5000, 300000, [&](int i) {
        flux_async1_fast_runner(i);
        drain(io_full_flux);
    });
    print_result(r9b);

    auto bp_async1_fastawait = make_async_1_fast_bp(&ex_full_post);
    auto bp_async1_fastawait_ptr = make_lite_ptr<decltype(bp_async1_fastawait)>(std::move(bp_async1_fastawait));
    auto flux_async1_fastawait_runner = make_runner(bp_async1_fastawait_ptr, sink_receiver{&sink});
    auto r9i = run_bench("full.flux.on_asio.runner.fast_awaitable.async1.post", 5000, 300000, [&](int i) {
        flux_async1_fastawait_runner(i);
        drain(io_full_flux);
    });
    print_result(r9i);

    auto bp_async1_fastawait_fast = make_async_1_fast_bp(&ex_full_post);
    auto flux_async1_fastawait_fast_runner = make_fast_runner_view(bp_async1_fastawait_fast, sink_receiver{&sink});
    auto r9j = run_bench("full.flux.on_asio.fast_runner.fast_awaitable.async1.post", 5000, 300000, [&](int i) {
        flux_async1_fastawait_fast_runner(i);
        drain(io_full_flux);
    });
    print_result(r9j);

    auto r9c = run_bench("full.asio.raw.post.async1", 5000, 300000, [&](int i) {
        asio_post_async1(io_full_asio, i, &sink);
    });
    print_result(r9c);

    auto r9c2 = run_bench("full.asio.adapter.post.async1", 5000, 300000, [&](int i) {
        asio_post_adapter_async1(ex_full_asio_post_adapter, io_full_asio, i, &sink);
    });
    print_result(r9c2);

    auto leaf1_all = make_blueprint<int, err_t>()
        | transform([](int x) noexcept { return x + 10; })
        | end();
    auto leaf2_all = make_blueprint<int, err_t>()
        | transform([](int x) noexcept { return x + 20; })
        | end();
    auto p1_all = make_lite_ptr<decltype(leaf1_all)>(std::move(leaf1_all));
    auto p2_all = make_lite_ptr<decltype(leaf2_all)>(std::move(leaf2_all));
    auto bp_all = await_when_all(
        &ex_full_post,
        [](int a, int b) noexcept { return out_t(value_tag, a + b); },
        [](flow_async_agg_err_t e) noexcept { return out_t(error_tag, std::move(e)); },
        p1_all, p2_all) | end();
    auto bp_all_ptr = make_lite_ptr<decltype(bp_all)>(std::move(bp_all));
    auto flux_when_all_runner = make_runner(bp_all_ptr, sink_receiver{&sink});

    auto r10 = run_bench("full.flux.on_asio.runner.when_all2.post", 5000, 120000, [&](int i) {
        flux_when_all_runner(i, i + 1);
        drain(io_full_flux);
    });
    print_result(r10);

    auto leaf1_all_fast = make_blueprint<int, err_t>()
        | transform([](int x) noexcept { return x + 10; })
        | end();
    auto leaf2_all_fast = make_blueprint<int, err_t>()
        | transform([](int x) noexcept { return x + 20; })
        | end();
    auto p1_all_fast = make_lite_ptr<decltype(leaf1_all_fast)>(std::move(leaf1_all_fast));
    auto p2_all_fast = make_lite_ptr<decltype(leaf2_all_fast)>(std::move(leaf2_all_fast));
    auto bp_all_fast = await_when_all(
        &ex_full_post,
        [](int a, int b) noexcept { return out_t(value_tag, a + b); },
        [](flow_async_agg_err_t e) noexcept { return out_t(error_tag, std::move(e)); },
        p1_all_fast, p2_all_fast) | end();
    auto flux_when_all_fast_runner = make_fast_runner_view(bp_all_fast, sink_receiver{&sink});
    auto r11 = run_bench("full.flux.on_asio.fast_runner.when_all2.post", 5000, 120000, [&](int i) {
        flux_when_all_fast_runner(i, i + 1);
        drain(io_full_flux);
    });
    print_result(r11);

    auto leaf1_all_ffast = make_blueprint<int, err_t>()
        | transform([](int x) noexcept { return x + 10; })
        | end();
    auto leaf2_all_ffast = make_blueprint<int, err_t>()
        | transform([](int x) noexcept { return x + 20; })
        | end();
    auto p1_all_ffast = make_lite_ptr<decltype(leaf1_all_ffast)>(std::move(leaf1_all_ffast));
    auto p2_all_ffast = make_lite_ptr<decltype(leaf2_all_ffast)>(std::move(leaf2_all_ffast));
    auto bp_all_ffast = await_when_all_fast(
        &ex_full_post,
        [](int a, int b) noexcept { return out_t(value_tag, a + b); },
        [](flow_async_agg_err_t e) noexcept { return out_t(error_tag, std::move(e)); },
        p1_all_ffast, p2_all_ffast) | end();
    auto flux_when_all_ffast_runner = make_fast_runner_view(bp_all_ffast, sink_receiver{&sink});
    auto r11b = run_bench("full.flux.on_asio.fast_runner.when_all2.fastagg.post", 5000, 120000, [&](int i) {
        flux_when_all_ffast_runner(i, i + 1);
        drain(io_full_flux);
    });
    print_result(r11b);

    auto r12 = run_bench("full.asio.raw.post.when_all2", 5000, 120000, [&](int i) {
        asio_post_when_all2(io_full_asio, i, i + 1, &sink);
    });
    print_result(r12);

    auto leaf1_any = make_blueprint<int, err_t>()
        | transform([](int x) noexcept { return x + 100; })
        | end();
    auto leaf2_any = make_blueprint<int, err_t>()
        | transform([](int x) noexcept { return x + 200; })
        | end();
    auto p1_any = make_lite_ptr<decltype(leaf1_any)>(std::move(leaf1_any));
    auto p2_any = make_lite_ptr<decltype(leaf2_any)>(std::move(leaf2_any));
    auto bp_any = await_when_any(
        &ex_full_post,
        [](size_t i, int v) noexcept { return out_t(value_tag, v); },
        [](flow_async_agg_err_t e) noexcept { return out_t(error_tag, std::move(e)); },
        p1_any, p2_any) | end();
    auto bp_any_ptr = make_lite_ptr<decltype(bp_any)>(std::move(bp_any));
    auto flux_when_any_runner = make_runner(bp_any_ptr, sink_receiver{&sink});

    auto r13 = run_bench("full.flux.on_asio.runner.when_any2.post", 5000, 120000, [&](int i) {
        flux_when_any_runner(i, i + 1);
        drain(io_full_flux);
    });
    print_result(r13);

    auto leaf1_any_fast = make_blueprint<int, err_t>()
        | transform([](int x) noexcept { return x + 100; })
        | end();
    auto leaf2_any_fast = make_blueprint<int, err_t>()
        | transform([](int x) noexcept { return x + 200; })
        | end();
    auto p1_any_fast = make_lite_ptr<decltype(leaf1_any_fast)>(std::move(leaf1_any_fast));
    auto p2_any_fast = make_lite_ptr<decltype(leaf2_any_fast)>(std::move(leaf2_any_fast));
    auto bp_any_fast = await_when_any(
        &ex_full_post,
        [](size_t i, int v) noexcept { return out_t(value_tag, v); },
        [](flow_async_agg_err_t e) noexcept { return out_t(error_tag, std::move(e)); },
        p1_any_fast, p2_any_fast) | end();
    auto flux_when_any_fast_runner = make_fast_runner_view(bp_any_fast, sink_receiver{&sink});
    auto r14 = run_bench("full.flux.on_asio.fast_runner.when_any2.post", 5000, 120000, [&](int i) {
        flux_when_any_fast_runner(i, i + 1);
        drain(io_full_flux);
    });
    print_result(r14);

    auto leaf1_any_ffast = make_blueprint<int, err_t>()
        | transform([](int x) noexcept { return x + 100; })
        | end();
    auto leaf2_any_ffast = make_blueprint<int, err_t>()
        | transform([](int x) noexcept { return x + 200; })
        | end();
    auto p1_any_ffast = make_lite_ptr<decltype(leaf1_any_ffast)>(std::move(leaf1_any_ffast));
    auto p2_any_ffast = make_lite_ptr<decltype(leaf2_any_ffast)>(std::move(leaf2_any_ffast));
    auto bp_any_ffast = await_when_any_fast(
        &ex_full_post,
        [](size_t i, int v) noexcept { return out_t(value_tag, v); },
        [](flow_async_agg_err_t e) noexcept { return out_t(error_tag, std::move(e)); },
        p1_any_ffast, p2_any_ffast) | end();
    auto flux_when_any_ffast_runner = make_fast_runner_view(bp_any_ffast, sink_receiver{&sink});
    auto r14b = run_bench("full.flux.on_asio.fast_runner.when_any2.fastagg.post", 5000, 120000, [&](int i) {
        flux_when_any_ffast_runner(i, i + 1);
        drain(io_full_flux);
    });
    print_result(r14b);

    auto r15 = run_bench("full.asio.raw.post.when_any2", 5000, 120000, [&](int i) {
        asio_post_when_any2(io_full_asio, i, i + 1, &sink);
    });
    print_result(r15);

    std::printf("\n[real backend async overhead]\n");
    asio::thread_pool real_backend_pool(1);
    g_real_backend_pool = &real_backend_pool;

    sync_wait_state flux_real_done4;
    sync_wait_state flux_real_done1;
    std::atomic<int> asio_real_done4{0};
    std::atomic<int> asio_real_done1{0};

    auto bp_real_flux4 = make_async_4_real_backend_fast_bp();
    auto flux_real_backend_fast_runner4 = make_fast_runner(std::move(bp_real_flux4), sync_wait_receiver{&sink, &flux_real_done4});
    auto r16 = run_bench("real.flux.fast_runner.fast_awaitable.async4.backend", 2000, 120000, [&](int i) {
        flux_real_done4.done.store(0, std::memory_order_relaxed);
        flux_real_backend_fast_runner4(i);
        while (flux_real_done4.done.load(std::memory_order_acquire) == 0) {
            std::this_thread::yield();
        }
    });
    print_result(r16);

    auto r17 = run_bench("real.asio.raw.async4.backend", 2000, 120000, [&](int i) {
        asio_real_done4.store(0, std::memory_order_relaxed);
        asio_real_backend_async4(real_backend_pool, i, &sink, asio_real_done4);
        while (asio_real_done4.load(std::memory_order_acquire) == 0) {
            std::this_thread::yield();
        }
    });
    print_result(r17);

    auto bp_real_flux1 = make_async_1_real_backend_fast_bp();
    auto flux_real_backend_fast_runner1 = make_fast_runner(std::move(bp_real_flux1), sync_wait_receiver{&sink, &flux_real_done1});
    auto r18 = run_bench("real.flux.fast_runner.fast_awaitable.async1.backend", 2000, 200000, [&](int i) {
        flux_real_done1.done.store(0, std::memory_order_relaxed);
        flux_real_backend_fast_runner1(i);
        while (flux_real_done1.done.load(std::memory_order_acquire) == 0) {
            std::this_thread::yield();
        }
    });
    print_result(r18);

    auto r19 = run_bench("real.asio.raw.async1.backend", 2000, 200000, [&](int i) {
        asio_real_done1.store(0, std::memory_order_relaxed);
        asio_real_backend_async1(real_backend_pool, i, &sink, asio_real_done1);
        while (asio_real_done1.load(std::memory_order_acquire) == 0) {
            std::this_thread::yield();
        }
    });
    print_result(r19);

    real_backend_pool.join();
    g_real_backend_pool = nullptr;

    std::printf("sink=%lld\n", sink);
    return 0;
}
