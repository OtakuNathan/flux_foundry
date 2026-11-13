// Diagnostic executable only. Linker wrappers count calls originating in the
// executable, not allocations internal to dynamically linked libraries.
// Tracking is single-threaded and limited to the timed owner-thread window.
#include "diagnostic_alloc.h"
#include <cstddef>
#include <cstdio>
#include <new>
namespace {
thread_local bool active = false;
thread_local std::size_t mallocs = 0, aligned = 0, news = 0;
}
extern "C" void ff_alloc_begin() { mallocs = aligned = news = 0; active = true; }
extern "C" void ff_alloc_end() {
    active = false;
    std::fprintf(stderr, "alloc_calls malloc=%zu aligned=%zu global_new=%zu\n", mallocs, aligned, news);
}
extern "C" void* __real_malloc(std::size_t);
extern "C" void* __wrap_malloc(std::size_t n) {
    if (active) ++mallocs;
    return __real_malloc(n);
}
extern "C" int __real_posix_memalign(void**, std::size_t, std::size_t);
extern "C" int __wrap_posix_memalign(void** p, std::size_t align, std::size_t n) {
    if (active) ++aligned;
    return __real_posix_memalign(p, align, n);
}
extern "C" void* __real_aligned_alloc(std::size_t, std::size_t);
extern "C" void* __wrap_aligned_alloc(std::size_t align, std::size_t n) {
    if (active) ++aligned;
    return __real_aligned_alloc(align, n);
}
extern "C" void* __real__Znwm(std::size_t);
extern "C" void* __wrap__Znwm(std::size_t n) {
    if (active) ++news;
    return __real__Znwm(n);
}
extern "C" void* __real__ZnwmRKSt9nothrow_t(std::size_t, const std::nothrow_t&);
extern "C" void* __wrap__ZnwmRKSt9nothrow_t(std::size_t n, const std::nothrow_t& tag) {
    if (active) ++news;
    return __real__ZnwmRKSt9nothrow_t(n, tag);
}
