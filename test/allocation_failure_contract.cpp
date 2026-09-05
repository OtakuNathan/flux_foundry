#include "memory/lite_ptr.h"
#include <cstdio>
#include <stdexcept>

using namespace flux_foundry;
struct counts { int attempts = 0; int live = 0; int fail_at = 0; };
struct failing_allocator {
    counts* state;
    void* allocate(size_t align, size_t size) const noexcept {
        if (++state->attempts == state->fail_at) return nullptr;
        auto* p = flux_foundry::aligned_alloc(align, size);
        if (p) ++state->live;
        return p;
    }
    void deallocate(void* p) const noexcept {
        if (p) --state->live;
        aligned_free(p);
    }
};
struct payload {
    static int live;
    explicit payload(bool fail)
#if !FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        noexcept
#endif
    {
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        if (fail) throw std::runtime_error("injected construction failure");
#else
        (void)fail;
#endif
        ++live;
    }
    ~payload() { --live; }
};
int payload::live = 0;

int main() {
    bool ok = true;
    for (int fail_at = 1; fail_at <= 3; ++fail_at) {
        counts state;
        state.fail_at = fail_at;
        int failures = 0;
        for (int i = 0; i < 3; ++i) {
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            try {
#endif
            {
                auto p = make_lite_ptr_with_allocator<payload>(failing_allocator{&state}, false);
                if (p) ok = ok && state.live == 1 && payload::live == 1;
                else {
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
                    ok = false; // Exception builds must throw on allocation failure.
#else
                    ++failures;
#endif
                }
            }
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            } catch (const std::bad_alloc&) { ++failures; }
#endif
            ok = ok && state.live == 0 && payload::live == 0;
        }
        ok = ok && failures == 1;
    }
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    counts state;
    bool caught = false;
    try {
        auto p = make_lite_ptr_with_allocator<payload>(failing_allocator{&state}, true);
    } catch (const std::runtime_error&) { caught = true; }
    ok = ok && caught && state.attempts == 1 && state.live == 0 && payload::live == 0;
#endif
    std::printf("[%s] allocation failure and throwing constructor cleanup\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
