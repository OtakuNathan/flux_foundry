#include <chrono>
#include <cstdio>
#include <exception>
#include <utility>

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

    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    bench_result r{};
    r.name = name;
    r.warmup = warmup;
    r.iters = iters;
    r.elapsed_ns = ns;
    r.ns_per_op = static_cast<double>(ns) / static_cast<double>(iters);
    return r;
}

void print_result(const bench_result& r) {
    std::printf("%-24s warmup=%-8d iter=%-8d total=%.3f ms  ns/op=%.2f\n",
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

auto make_async_4_bp(inline_executor* ex) {
    auto bp = make_blueprint<int>()
        | await<immediate_plus_one_awaitable>(ex)
        | await<immediate_plus_one_awaitable>(ex)
        | await<immediate_plus_one_awaitable>(ex)
        | await<immediate_plus_one_awaitable>(ex)
        | end();
    return bp;
}

} // namespace

int main() {
    std::printf("[flow perf] compiler baseline: clang++ -std=c++14 -O3\n");

    volatile long long sink = 0;
    inline_executor ex;

    auto r0 = run_bench("direct.loop20", 20000, 5000000, [&](int i) {
        int x = i;
        x += 1; x += 1; x += 1; x += 1; x += 1;
        x += 1; x += 1; x += 1; x += 1; x += 1;
        x += 1; x += 1; x += 1; x += 1; x += 1;
        x += 1; x += 1; x += 1; x += 1; x += 1;
        sink += static_cast<long long>(x);
    });
    print_result(r0);

    auto bp_sync_std = make_sync_20_bp();
    auto bp_sync_std_ptr = make_lite_ptr<decltype(bp_sync_std)>(std::move(bp_sync_std));
    auto std_runner = make_runner(bp_sync_std_ptr, sink_receiver{&sink});

    auto r1 = run_bench("runner.sync.20nodes", 20000, 2000000, [&](int i) {
        std_runner(i);
    });
    print_result(r1);

    auto bp_sync_fast = make_sync_20_bp();
    auto fast_runner = make_fast_runner(std::move(bp_sync_fast), sink_receiver{&sink});

    auto r2 = run_bench("fast_runner.sync.20nodes", 20000, 3000000, [&](int i) {
        fast_runner(i);
    });
    print_result(r2);

    auto bp_async = make_async_4_bp(&ex);
    auto bp_async_ptr = make_lite_ptr<decltype(bp_async)>(std::move(bp_async));
    auto async_runner = make_runner(bp_async_ptr, sink_receiver{&sink});

    auto r3 = run_bench("runner.async.4nodes", 10000, 800000, [&](int i) {
        async_runner(i);
    });
    print_result(r3);

    auto leaf1_all = make_blueprint<int>()
        | transform([](int x) noexcept { return x + 10; })
        | end();
    auto leaf2_all = make_blueprint<int>()
        | transform([](int x) noexcept { return x + 20; })
        | end();

    auto p1_all = make_lite_ptr<decltype(leaf1_all)>(std::move(leaf1_all));
    auto p2_all = make_lite_ptr<decltype(leaf2_all)>(std::move(leaf2_all));

    auto bp_all = await_when_all(
        &ex,
        [](int a, int b) noexcept {
            return out_t(value_tag, a + b);
        },
        [](flow_async_agg_err_t e) noexcept {
            return out_t(error_tag, std::move(e));
        },
        p1_all,
        p2_all)
        | end();

    auto bp_all_ptr = make_lite_ptr<decltype(bp_all)>(std::move(bp_all));
    auto when_all_runner = make_runner(bp_all_ptr, sink_receiver{&sink});

    auto r4 = run_bench("runner.when_all.2", 5000, 300000, [&](int i) {
        when_all_runner(make_flat_storage(i, i + 1));
    });
    print_result(r4);

    auto leaf1_any = make_blueprint<int>()
        | transform([](int x) noexcept { return x + 100; })
        | end();
    auto leaf2_any = make_blueprint<int>()
        | transform([](int x) noexcept { return x + 200; })
        | end();

    auto p1_any = make_lite_ptr<decltype(leaf1_any)>(std::move(leaf1_any));
    auto p2_any = make_lite_ptr<decltype(leaf2_any)>(std::move(leaf2_any));

    auto bp_any = await_when_any(
        &ex,
        [](int v) noexcept {
            return out_t(value_tag, v);
        },
        [](flow_async_agg_err_t e) noexcept {
            return out_t(error_tag, std::move(e));
        },
        p1_any,
        p2_any)
        | end();

    auto bp_any_ptr = make_lite_ptr<decltype(bp_any)>(std::move(bp_any));
    auto when_any_runner = make_runner(bp_any_ptr, sink_receiver{&sink});

    auto r5 = run_bench("runner.when_any.2", 5000, 300000, [&](int i) {
        when_any_runner(make_flat_storage(i, i + 1));
    });
    print_result(r5);

    std::printf("sink=%lld\n", sink);
    return 0;
}
