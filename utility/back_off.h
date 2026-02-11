#ifndef FLUX_FOUNDRY_PAUSE_H
#define FLUX_FOUNDRY_PAUSE_H

#include <thread>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#include <algorithm>
#elif defined(__aarch64__)
#else
#endif

namespace flux_foundry {
    template <size_t spin_limit = 16, size_t max_loop = 1024>
    struct backoff_strategy {
        size_t count {1};
        size_t steps {0};

        void reset() noexcept {
            count = 1;
            steps = 0;
        }

        void yield() noexcept {
            if (steps < spin_limit) {
                for (size_t i = 0; i < count; ++i) {
#if defined(__x86_64__) || defined(_M_X64)
                    _mm_pause();
#elif defined(__aarch64__)
                    __asm__ __volatile__("yield");
#else
                    std::atomic_signal_fence(std::memory_order_relaxed);
#endif
                }
                count = std::min(count << 1, max_loop);
                ++steps;
            } else {
                std::this_thread::yield();
            }
        }
    };
}

#endif
