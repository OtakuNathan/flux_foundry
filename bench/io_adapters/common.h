#pragma once
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <sched.h>

namespace probe {
using clock = std::chrono::steady_clock;
inline void require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "%s\n", message); std::abort(); }
}
inline void pin(unsigned cpu) {
    require(cpu < CPU_SETSIZE, "CPU exceeds affinity mask");
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    require(pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask) == 0,
            "cannot pin requested CPU");
}
inline double ns(clock::time_point begin, clock::time_point end) {
    return std::chrono::duration<double, std::nano>(end - begin).count();
}
} // namespace probe
