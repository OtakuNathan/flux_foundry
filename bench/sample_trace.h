#ifndef FF_BENCH_SAMPLE_TRACE_H
#define FF_BENCH_SAMPLE_TRACE_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <vector>

namespace ff_bench {
// Called after the measurement and FIFO fence. No added hot-path timestamps.
// A caller-selected prefix keeps separate processes/cases from overwriting data.
template<class Sample>
void export_samples(const std::vector<Sample>& samples, std::int64_t origin,
                    std::size_t capacity, const char* mode, const char* profile,
                    unsigned repeat) {
    static_assert(std::is_same<decltype(Sample::planned), std::int64_t>::value &&
                  std::is_same<decltype(Sample::submit_start), std::int64_t>::value &&
                  std::is_same<decltype(Sample::submit_end), std::int64_t>::value &&
                  std::is_same<decltype(Sample::dispatch_start), std::int64_t>::value &&
                  std::is_same<decltype(Sample::dispatch_end), std::int64_t>::value &&
                  std::is_same<decltype(Sample::begin), std::int64_t>::value &&
                  std::is_same<decltype(Sample::end), std::int64_t>::value,
                  "sample traces require signed 64-bit nanosecond timestamps");
    const char* prefix = std::getenv("FF_BENCH_TRACE_PREFIX");
    if (!prefix || !*prefix || repeat == 0) return; // Exclude warmup.
    const auto path = std::string(prefix) + "-" + std::to_string(capacity) + "-" +
        mode + "-" + profile + "-" + std::to_string(repeat) + ".samples.csv";
    auto* file = std::fopen(path.c_str(), "w");
    if (!file) { std::perror(path.c_str()); std::abort(); }
    std::fputs("id,planned_ns,submit_start_ns,submit_end_ns,dispatch_start_ns,dispatch_end_ns,begin_ns,end_ns\n", file);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto& s = samples[i];
        std::fprintf(file, "%zu,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n", i,
            static_cast<long long>(s.planned - origin),
            static_cast<long long>(s.submit_start - origin),
            static_cast<long long>(s.submit_end - origin),
            static_cast<long long>(s.dispatch_start - origin),
            static_cast<long long>(s.dispatch_end - origin),
            static_cast<long long>(s.begin - origin),
            static_cast<long long>(s.end - origin));
    }
    bool ok = !std::ferror(file);
    if (std::fclose(file) != 0) ok = false;
    if (!ok) { std::fprintf(stderr, "cannot write trace: %s\n", path.c_str()); std::abort(); }
}
}
#endif
