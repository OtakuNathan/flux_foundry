// Compare controller storage with identical cancellation-capable execution.
// Define FF_CONTROLLER_TRACE_ALLOC and link --wrap=posix_memalign for allocation
// counts only; those instrumented runs are not reported as timings.
#include "test/lifecycle_test_support.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef FF_CONTROLLER_TRACE_ALLOC
static bool tracing = false;
static std::uint64_t aligned_calls = 0;
extern "C" int __real_posix_memalign(void**, size_t, size_t);
extern "C" int __wrap_posix_memalign(void** ptr, size_t alignment, size_t bytes) {
    if (tracing) ++aligned_calls;
    return __real_posix_memalign(ptr, alignment, bytes);
}
#endif

using namespace flux_foundry;
namespace {
using error = lifecycle_test::error;
using output = result_t<std::uint64_t, error>;
struct totals { std::uint64_t count = 0, sum = 0; };
struct receiver {
    using value_type = output;
    totals* out;
    void emplace(output&& value) noexcept {
        if (!value.has_value()) std::abort();
        ++out->count;
        out->sum += value.value();
    }
};

template <typename BP>
void invoke(lite_ptr<BP>& bp, totals& seen, std::uint64_t i, std::integral_constant<int, 0>) {
    auto runner = make_runner(bp, receiver{&seen});
    runner(i);
}
template <typename BP>
void invoke(lite_ptr<BP>& bp, totals& seen, std::uint64_t i, std::integral_constant<int, 1>) {
    auto runner = make_runner(std::allocator_arg, aligned_malloc_allocator{}, bp, receiver{&seen});
    runner(i);
}
template <typename BP>
void invoke(lite_ptr<BP>& bp, totals& seen, std::uint64_t i, std::integral_constant<int, 2>) {
    auto runner = make_runner(std::allocator_arg, pooled_controller_allocator<>{}, bp, receiver{&seen});
    runner(i);
}

template <int Policy>
void run(std::size_t iterations) {
    const char* policy = Policy == 0 ? "system" : Policy == 1 ? "system-eager" : "pooled";
    auto bp = make_blueprint<std::uint64_t, error>()
        | transform([](std::uint64_t value) noexcept { return value + 1; }) | end();
    auto shared = make_lite_ptr<decltype(bp)>(std::move(bp));
    totals seen;
    for (std::size_t i = 0; i < 4096; ++i) invoke(shared, seen, i, std::integral_constant<int, Policy>{});
    seen = {};
#ifdef FF_CONTROLLER_TRACE_ALLOC
    aligned_calls = 0;
    tracing = true;
#endif
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) invoke(shared, seen, i, std::integral_constant<int, Policy>{});
    const auto finish = std::chrono::steady_clock::now();
#ifdef FF_CONTROLLER_TRACE_ALLOC
    tracing = false;
#endif
    if (seen.count != iterations || seen.sum != std::uint64_t(iterations) * (iterations + 1) / 2) std::abort();
    const auto elapsed = std::chrono::duration<double, std::nano>(finish - start).count() / iterations;
    std::puts("policy,iterations,mode,ns_per_op,aligned_allocations,count,checksum");
#ifdef FF_CONTROLLER_TRACE_ALLOC
    (void)elapsed;
    std::printf("%s,%zu,allocation_trace,,%llu,%llu,%llu\n", policy, iterations,
        static_cast<unsigned long long>(aligned_calls),
        static_cast<unsigned long long>(seen.count), static_cast<unsigned long long>(seen.sum));
#else
    std::printf("%s,%zu,timing,%.6f,,%llu,%llu\n", policy, iterations, elapsed,
        static_cast<unsigned long long>(seen.count), static_cast<unsigned long long>(seen.sum));
#endif
}
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) return 2;
    const auto iterations = argc == 3 ? std::strtoull(argv[2], nullptr, 10) : 1000000;
    if (!iterations || iterations > 1000000000) return 2;
    std::thread([] {}).join();
    if (!std::strcmp(argv[1], "system")) run<0>(iterations);
    else if (!std::strcmp(argv[1], "system-eager")) run<1>(iterations);
    else if (!std::strcmp(argv[1], "pooled")) run<2>(iterations);
    else return 2;
}
