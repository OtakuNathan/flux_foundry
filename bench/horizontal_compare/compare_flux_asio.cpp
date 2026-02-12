#include <chrono>
#include <cstdio>
#include <exception>
#include <utility>

#define ASIO_STANDALONE 1
#include <asio.hpp>

#include "flow/flow.h"

using namespace flux_foundry;

namespace {

using err_t = std::exception_ptr;
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

struct immediate_plus_one_awaitable : awaitable_base<immediate_plus_one_awaitable, int, err_t> {
    using async_result_type = out_t;

    int v;

    explicit immediate_plus_one_awaitable(async_result_type&& in) noexcept
        : v(in.has_value() ? in.value() : 0) {
    }

    int submit() noexcept {
        this->resume(async_result_type(value_tag, v + 1));
        return 0;
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
    auto bp = make_blueprint<int>()
        | transform([](int x) noexcept { return x + 1; })
        | transform([](int x) noexcept { return x + 1; })
        | transform([](int x) noexcept { return x + 1; })
        | transform([](int x) noexcept { return x + 1; })
        | transform([](int x) noexcept { return x + 1; })
        | transform([](int x) noexcept { return x + 1; })
        | transform([](int x) noexcept { return x + 1; })
        | transform([](int x) noexcept { return x + 1; })
        | transform([](int x) noexcept { return x + 1; })
        | transform([](int x) noexcept { return x + 1; })
        | transform([](int x) noexcept { return x + 1; })
        | transform([](int x) noexcept { return x + 1; })
        | transform([](int x) noexcept { return x + 1; })
        | transform([](int x) noexcept { return x + 1; })
        | transform([](int x) noexcept { return x + 1; })
        | transform([](int x) noexcept { return x + 1; })
        | transform([](int x) noexcept { return x + 1; })
        | transform([](int x) noexcept { return x + 1; })
        | transform([](int x) noexcept { return x + 1; })
        | transform([](int x) noexcept { return x + 1; })
        | end();
    return bp;
}

template <typename ExecutorPtr>
auto make_via_20_bp(ExecutorPtr ex) {
    auto bp = make_blueprint<int>()
        | via(ex) | via(ex) | via(ex) | via(ex) | via(ex)
        | via(ex) | via(ex) | via(ex) | via(ex) | via(ex)
        | via(ex) | via(ex) | via(ex) | via(ex) | via(ex)
        | via(ex) | via(ex) | via(ex) | via(ex) | via(ex)
        | end();
    return bp;
}

template <typename ExecutorPtr>
auto make_async_4_bp(ExecutorPtr ex) {
    auto bp = make_blueprint<int>()
        | await<immediate_plus_one_awaitable>(ex)
        | await<immediate_plus_one_awaitable>(ex)
        | await<immediate_plus_one_awaitable>(ex)
        | await<immediate_plus_one_awaitable>(ex)
        | end();
    return bp;
}

inline void drain(asio::io_context& io) {
    io.restart();
    io.run();
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

void asio_post_async4(asio::io_context& io, int in, volatile long long* sink) {
    asio::post(io, asio_post_async4_hop{&io, sink, in, 4});
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

} // namespace

int main() {
    std::printf("[horizontal compare] flux_foundry vs asio (same host/toolchain)\n");
    std::printf("[build] clang++ -std=c++14 -O3 -DNDEBUG\n");

    volatile long long sink = 0;
    asio::io_context io_sched_dispatch;
    asio::io_context io_sched_post;
    asio::io_context io_full_flux;
    asio::io_context io_full_asio;

    asio_dispatch_adapter ex_sched_dispatch{&io_sched_dispatch};
    asio_post_adapter ex_sched_post{&io_sched_post};
    asio_post_adapter ex_full_post{&io_full_flux};

    auto r0 = run_bench("baseline.direct.loop20", 10000, 3000000, [&](int i) {
        int x = i;
        x += 1; x += 1; x += 1; x += 1; x += 1;
        x += 1; x += 1; x += 1; x += 1; x += 1;
        x += 1; x += 1; x += 1; x += 1; x += 1;
        x += 1; x += 1; x += 1; x += 1; x += 1;
        sink += static_cast<long long>(x);
    });
    print_result(r0);

    std::printf("\n[pure scheduling overhead]\n");
    auto bp_via_dispatch = make_via_20_bp(&ex_sched_dispatch);
    auto bp_via_dispatch_ptr = make_lite_ptr<decltype(bp_via_dispatch)>(std::move(bp_via_dispatch));
    auto flux_via_dispatch_runner = make_runner(bp_via_dispatch_ptr, sink_receiver{&sink});
    auto r1 = run_bench("sched.flux.via20.dispatch", 5000, 200000, [&](int i) {
        flux_via_dispatch_runner(i);
        drain(io_sched_dispatch);
    });
    print_result(r1);

    auto r2 = run_bench("sched.asio.dispatch20", 5000, 200000, [&](int i) {
        asio_dispatch_sync20(io_sched_dispatch, i, &sink);
    });
    print_result(r2);

    auto bp_via_post = make_via_20_bp(&ex_sched_post);
    auto bp_via_post_ptr = make_lite_ptr<decltype(bp_via_post)>(std::move(bp_via_post));
    auto flux_via_post_runner = make_runner(bp_via_post_ptr, sink_receiver{&sink});
    auto r3 = run_bench("sched.flux.via20.post", 5000, 120000, [&](int i) {
        flux_via_post_runner(i);
        drain(io_sched_post);
    });
    print_result(r3);

    auto r4 = run_bench("sched.asio.post20", 5000, 120000, [&](int i) {
        asio_post_sync20(io_sched_post, i, &sink);
    });
    print_result(r4);

    std::printf("\n[full semantics overhead]\n");
    auto bp_sync_std = make_sync_20_bp();
    auto bp_sync_std_ptr = make_lite_ptr<decltype(bp_sync_std)>(std::move(bp_sync_std));
    auto flux_sync_runner = make_runner(bp_sync_std_ptr, sink_receiver{&sink});
    auto r5 = run_bench("full.flux.runner.sync20", 10000, 800000, [&](int i) {
        flux_sync_runner(i);
    });
    print_result(r5);

    auto bp_sync_fast = make_sync_20_bp();
    auto flux_fast_runner = make_fast_runner(std::move(bp_sync_fast), sink_receiver{&sink});
    auto r6 = run_bench("full.flux.fast_runner.sync20", 10000, 1200000, [&](int i) {
        flux_fast_runner(i);
    });
    print_result(r6);

    auto bp_async = make_async_4_bp(&ex_full_post);
    auto bp_async_ptr = make_lite_ptr<decltype(bp_async)>(std::move(bp_async));
    auto flux_async_runner = make_runner(bp_async_ptr, sink_receiver{&sink});
    auto r7 = run_bench("full.flux.runner.async4.post", 5000, 180000, [&](int i) {
        flux_async_runner(i);
        drain(io_full_flux);
    });
    print_result(r7);

    auto bp_async_fast = make_async_4_bp(&ex_full_post);
    auto flux_async_fast_runner = make_fast_runner_view(bp_async_fast, sink_receiver{&sink});
    auto r8 = run_bench("full.flux.fast_runner.async4.post", 5000, 180000, [&](int i) {
        flux_async_fast_runner(i);
        drain(io_full_flux);
    });
    print_result(r8);

    auto r9 = run_bench("full.asio.post.async4", 5000, 180000, [&](int i) {
        asio_post_async4(io_full_asio, i, &sink);
    });
    print_result(r9);

    auto leaf1_all = make_blueprint<int>()
        | transform([](int x) noexcept { return x + 10; })
        | end();
    auto leaf2_all = make_blueprint<int>()
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

    auto r10 = run_bench("full.flux.runner.when_all2.post", 5000, 120000, [&](int i) {
        flux_when_all_runner(make_flat_storage(i, i + 1));
        drain(io_full_flux);
    });
    print_result(r10);

    auto leaf1_all_fast = make_blueprint<int>()
        | transform([](int x) noexcept { return x + 10; })
        | end();
    auto leaf2_all_fast = make_blueprint<int>()
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
    auto r11 = run_bench("full.flux.fast_runner.when_all2.post", 5000, 120000, [&](int i) {
        flux_when_all_fast_runner(make_flat_storage(i, i + 1));
        drain(io_full_flux);
    });
    print_result(r11);

    auto r12 = run_bench("full.asio.post.when_all2", 5000, 120000, [&](int i) {
        asio_post_when_all2(io_full_asio, i, i + 1, &sink);
    });
    print_result(r12);

    auto leaf1_any = make_blueprint<int>()
        | transform([](int x) noexcept { return x + 100; })
        | end();
    auto leaf2_any = make_blueprint<int>()
        | transform([](int x) noexcept { return x + 200; })
        | end();
    auto p1_any = make_lite_ptr<decltype(leaf1_any)>(std::move(leaf1_any));
    auto p2_any = make_lite_ptr<decltype(leaf2_any)>(std::move(leaf2_any));
    auto bp_any = await_when_any(
        &ex_full_post,
        [](int v) noexcept { return out_t(value_tag, v); },
        [](flow_async_agg_err_t e) noexcept { return out_t(error_tag, std::move(e)); },
        p1_any, p2_any) | end();
    auto bp_any_ptr = make_lite_ptr<decltype(bp_any)>(std::move(bp_any));
    auto flux_when_any_runner = make_runner(bp_any_ptr, sink_receiver{&sink});

    auto r13 = run_bench("full.flux.runner.when_any2.post", 5000, 120000, [&](int i) {
        flux_when_any_runner(make_flat_storage(i, i + 1));
        drain(io_full_flux);
    });
    print_result(r13);

    auto leaf1_any_fast = make_blueprint<int>()
        | transform([](int x) noexcept { return x + 100; })
        | end();
    auto leaf2_any_fast = make_blueprint<int>()
        | transform([](int x) noexcept { return x + 200; })
        | end();
    auto p1_any_fast = make_lite_ptr<decltype(leaf1_any_fast)>(std::move(leaf1_any_fast));
    auto p2_any_fast = make_lite_ptr<decltype(leaf2_any_fast)>(std::move(leaf2_any_fast));
    auto bp_any_fast = await_when_any(
        &ex_full_post,
        [](int v) noexcept { return out_t(value_tag, v); },
        [](flow_async_agg_err_t e) noexcept { return out_t(error_tag, std::move(e)); },
        p1_any_fast, p2_any_fast) | end();
    auto flux_when_any_fast_runner = make_fast_runner_view(bp_any_fast, sink_receiver{&sink});
    auto r14 = run_bench("full.flux.fast_runner.when_any2.post", 5000, 120000, [&](int i) {
        flux_when_any_fast_runner(make_flat_storage(i, i + 1));
        drain(io_full_flux);
    });
    print_result(r14);

    auto r15 = run_bench("full.asio.post.when_any2", 5000, 120000, [&](int i) {
        asio_post_when_any2(io_full_asio, i, i + 1, &sink);
    });
    print_result(r15);

    std::printf("sink=%lld\n", sink);
    return 0;
}
