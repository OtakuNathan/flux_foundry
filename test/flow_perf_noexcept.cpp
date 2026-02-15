#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstdio>
#include <system_error>
#include <utility>

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

using namespace flux_foundry;

namespace {
using err_t = std::error_code;
using out_t = result_t<int, err_t>;

constexpr int kBenchRounds = 7;
constexpr long long kBenchMinRoundNs = 50LL * 1000 * 1000;

struct inline_executor {
    void dispatch(task_wrapper_sbo t) noexcept {
        t();
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

    void cancel() noexcept {
    }

    bool available() const noexcept {
        return true;
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
    int rounds;
    long long elapsed_ns;
    double ns_per_op;
    double p95_ns_per_op;
    double mean_ns_per_op;
};

template <typename F>
bench_result run_bench(const char* name, int warmup, int iters_hint, F&& fn) {
    for (int i = 0; i < warmup; ++i) {
        fn(i);
    }

    int iters = iters_hint > 0 ? iters_hint : 1;
    auto run_once = [&](int n) {
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) {
            fn(i);
        }
        auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    };

    long long calib_ns = run_once(iters);
    for (int attempt = 0; attempt < 6 && calib_ns < kBenchMinRoundNs && iters < (INT_MAX / 2); ++attempt) {
        const double scale = static_cast<double>(kBenchMinRoundNs) /
                             static_cast<double>(calib_ns > 0 ? calib_ns : 1);
        int next_iters = static_cast<int>(static_cast<double>(iters) * scale * 1.10);
        if (next_iters <= iters) {
            next_iters = iters * 2;
        }
        if (next_iters <= 0 || next_iters > (INT_MAX / 2)) {
            next_iters = INT_MAX / 2;
        }
        iters = next_iters;
        calib_ns = run_once(iters);
    }

    std::array<double, kBenchRounds> samples{};
    long long elapsed_sum_ns = 0;
    for (int r_i = 0; r_i < kBenchRounds; ++r_i) {
        const auto ns = run_once(iters);
        elapsed_sum_ns += ns;
        samples[r_i] = static_cast<double>(ns) / static_cast<double>(iters);
    }

    std::array<double, kBenchRounds> sorted = samples;
    std::sort(sorted.begin(), sorted.end());

    const double median = sorted[kBenchRounds / 2];
    int p95_index = ((kBenchRounds * 95) + 99) / 100 - 1;
    if (p95_index < 0) p95_index = 0;
    if (p95_index >= kBenchRounds) p95_index = kBenchRounds - 1;

    double mean = 0.0;
    for (double v : samples) {
        mean += v;
    }
    mean /= static_cast<double>(kBenchRounds);

    bench_result r{};
    r.name = name;
    r.warmup = warmup;
    r.iters = iters;
    r.rounds = kBenchRounds;
    r.elapsed_ns = elapsed_sum_ns / kBenchRounds;
    r.ns_per_op = median;
    r.p95_ns_per_op = sorted[p95_index];
    r.mean_ns_per_op = mean;
    return r;
}

void print_result(const bench_result& r) {
    std::printf("%-24s warmup=%-8d iter=%-8d rounds=%-2d total=%.3f ms/round  ns/op=%.2f  p95=%.2f  mean=%.2f\n",
        r.name,
        r.warmup,
        r.iters,
        r.rounds,
        static_cast<double>(r.elapsed_ns) / 1e6,
        r.ns_per_op,
        r.p95_ns_per_op,
        r.mean_ns_per_op);
}

auto make_sync_20_bp() {
    auto bp = make_blueprint<int, err_t>()
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
    auto bp = make_blueprint<int, err_t>()
        | await<immediate_plus_one_awaitable>(ex)
        | await<immediate_plus_one_awaitable>(ex)
        | await<immediate_plus_one_awaitable>(ex)
        | await<immediate_plus_one_awaitable>(ex)
        | end();
    return bp;
}

} // namespace

int main() {
    std::printf("[flow perf noexc strict] compiler baseline: -std=c++14 -O3 -fno-exceptions -DFLUEX_FOUNDRY_NO_EXCEPTION_STRICT=1 -I./ -fno-rtti -march=native -fstrict-aliasing\n");

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

    auto bp_async_fast = make_async_4_bp(&ex);
    auto async_fast_runner = make_fast_runner_view(bp_async_fast, sink_receiver{&sink});
    auto r7 = run_bench("fast_runner.async.4nodes", 10000, 800000, [&](int i) {
        async_fast_runner(i);
    });
    print_result(r7);

    auto leaf1_all = make_blueprint<int, err_t>()
        | transform([](int x) noexcept { return x + 10; })
        | end();
    auto leaf2_all = make_blueprint<int, err_t>()
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
            return out_t(error_tag, e);
        },
        p1_all,
        p2_all)
        | end();

    auto bp_all_ptr = make_lite_ptr<decltype(bp_all)>(std::move(bp_all));
    auto when_all_runner = make_runner(bp_all_ptr, sink_receiver{&sink});

    auto r4 = run_bench("runner.when_all.2", 5000, 300000, [&](int i) {
        when_all_runner(i, i + 1);
    });
    print_result(r4);

    auto bp_all_fast = await_when_all_fast(
        &ex,
        [](int a, int b) noexcept {
            return out_t(value_tag, a + b);
        },
        [](flow_async_agg_err_t e) noexcept {
            return out_t(error_tag, e);
        },
        p1_all,
        p2_all)
        | end();

    auto when_all_ffast_runner = make_fast_runner_view(bp_all_fast, sink_receiver{&sink});
    auto r4ff = run_bench("fast_runner.when_all_fast.2", 5000, 300000, [&](int i) {
        when_all_ffast_runner(i, i + 1);
    });
    print_result(r4ff);

    auto bp_all_fast_ptr = make_lite_ptr<decltype(bp_all_fast)>(std::move(bp_all_fast));
    auto when_all_fast_runner = make_runner(bp_all_fast_ptr, sink_receiver{&sink});

    auto r4f = run_bench("runner.when_all_fast.2", 5000, 300000, [&](int i) {
        when_all_fast_runner(i, i + 1);
    });
    print_result(r4f);

    auto leaf1_any = make_blueprint<int, err_t>()
        | transform([](int x) noexcept { return x + 100; })
        | end();
    auto leaf2_any = make_blueprint<int, err_t>()
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
            return out_t(error_tag, e);
        },
        p1_any,
        p2_any)
        | end();

    auto bp_any_ptr = make_lite_ptr<decltype(bp_any)>(std::move(bp_any));
    auto when_any_runner = make_runner(bp_any_ptr, sink_receiver{&sink});

    auto r5 = run_bench("runner.when_any.2", 5000, 300000, [&](int i) {
        when_any_runner(i, i + 1);
    });
    print_result(r5);

    auto bp_any_fast = await_when_any_fast(
        &ex,
        [](int v) noexcept {
            return out_t(value_tag, v);
        },
        [](flow_async_agg_err_t e) noexcept {
            return out_t(error_tag, e);
        },
        p1_any,
        p2_any)
        | end();

    auto when_any_ffast_runner = make_fast_runner_view(bp_any_fast, sink_receiver{&sink});
    auto r5ff = run_bench("fast_runner.when_any_fast.2", 5000, 300000, [&](int i) {
        when_any_ffast_runner(i, i + 1);
    });
    print_result(r5ff);

    auto bp_any_fast_ptr = make_lite_ptr<decltype(bp_any_fast)>(std::move(bp_any_fast));
    auto when_any_fast_runner = make_runner(bp_any_fast_ptr, sink_receiver{&sink});

    auto r5f = run_bench("runner.when_any_fast.2", 5000, 300000, [&](int i) {
        when_any_fast_runner(i, i + 1);
    });
    print_result(r5f);

    std::printf("sink=%lld\n", sink);
    return 0;
}
