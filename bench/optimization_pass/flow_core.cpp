// Focused mechanism probes, not representative network/service workloads.
// Compile: c++ -std=c++14 -O3 -DNDEBUG -pthread -I. bench/optimization_pass/flow_core.cpp -o /tmp/ff-flow-core
// Run one lane/process: /tmp/ff-flow-core continuation|aggregate [iterations]
#include "test/lifecycle_test_support.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

using namespace flux_foundry;
namespace {
using error = lifecycle_test::error;
using output = result_t<std::uint64_t, error>;
struct totals { std::uint64_t count = 0, checksum = 0; };
struct receiver {
    using value_type = output;
    std::shared_ptr<totals> sink; // Deliberately the common 16-byte receiver shape.
    void emplace(output&& out) noexcept {
        if (!out.has_value()) std::abort();
        ++sink->count;
        sink->checksum += out.value();
    }
};
struct executor {
    task_wrapper_sbo task;
    void dispatch(task_wrapper_sbo&& next) noexcept {
        if (task) std::abort();
        task = std::move(next);
    }
    void drain() noexcept {
        auto next = std::move(task);
        if (!next) std::abort();
        next();
    }
};
struct immediate final : awaitable_base<immediate, std::uint64_t, error> {
    using async_result_type = output;
    std::uint64_t value;
    explicit immediate(output&& in) noexcept : value(in.value()) {}
    bool available() const noexcept { return true; }
    void cancel() noexcept {}
    int submit() noexcept { resume(output(value_tag, value + 1)); return 0; }
};
struct report { double ns; std::uint64_t count; std::uint64_t checksum; };

template <typename Step>
report measure(std::size_t iterations, const std::shared_ptr<totals>& sink, Step step) {
    for (std::size_t i = 0; i < 4096; ++i) step(i);
    *sink = {};
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) step(i);
    const auto end = std::chrono::steady_clock::now();
    return {std::chrono::duration<double, std::nano>(end - begin).count() / iterations,
        sink->count, sink->checksum};
}

report continuation(std::size_t iterations) {
    executor ex;
    flow_controller controller; // Exclude controller allocation from this SBO comparison.
    auto bp = make_blueprint<std::uint64_t, error>() | await<immediate>(&ex) | end();
    auto ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto sink = std::make_shared<totals>();
    auto result = measure(iterations, sink, [&](std::uint64_t input) {
        flow_runner<decltype(bp), receiver, flow_controller*> runner(ptr, &controller, receiver{sink});
        runner(input);
        ex.drain();
    });
    const auto expected = std::uint64_t(iterations) * (iterations + 1) / 2;
    if (result.count != iterations || result.checksum != expected) std::abort();
    return result;
}

report aggregate(std::size_t iterations) {
    auto leaf = make_blueprint<std::uint64_t, error>()
        | transform([](std::uint64_t v) noexcept { return v + 1; }) | end();
    auto ptr = make_lite_ptr<decltype(leaf)>(std::move(leaf));
    auto bp = await_when_all_fast(
        [](std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) noexcept {
            return output(value_tag, a + b + c + d);
        },
        [](error e) noexcept { return output(error_tag, std::move(e)); }, ptr, ptr, ptr, ptr) | end();
    auto sink = std::make_shared<totals>();
    auto result = measure(iterations, sink, [&](std::uint64_t input) {
        auto runner = make_fast_runner_view(bp, receiver{sink});
        runner(input, input, input, input);
    });
    const auto expected = 2 * std::uint64_t(iterations) * (iterations + 1);
    if (result.count != iterations || result.checksum != expected) std::abort();
    return result;
}
} // namespace

int main(int argc, char** argv) {
    const std::size_t iterations = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1000000;
    if (argc < 2 || iterations == 0 || iterations > 1000000000) {
        std::fputs("usage: flow_core continuation|aggregate [iterations:1..1000000000]\n", stderr);
        return 2;
    }
    // Keep shared_ptr's multithread-aware implementation comparable between
    // this single-thread timing loop and production code that has threads.
    std::thread([] {}).join();
    report r{};
    if (std::strcmp(argv[1], "continuation") == 0) r = continuation(iterations);
    else if (std::strcmp(argv[1], "aggregate") == 0) r = aggregate(iterations);
    else return 2;
    std::printf("lane,iterations,ns_per_op,count,checksum\n%s,%zu,%.6f,%llu,%llu\n",
        argv[1], iterations, r.ns, static_cast<unsigned long long>(r.count),
        static_cast<unsigned long long>(r.checksum));
}
