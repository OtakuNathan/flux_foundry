#ifndef FLUX_FOUNDRY_POOLING_H
#define FLUX_FOUNDRY_POOLING_H

#include <new>
#include <cstdint>
#include <stdexcept>

#include "aligned_alloc.h"
#include "utility/static_list.h"
#include "base/traits.h"

namespace flux_foundry {
    // this pool only serves exact-type element_t allocations, no base/derived polymorphic allocations.
    // best-effort TLS cache; cross-thread frees may reduce locality and cause memory drift.
    template <typename element_t, size_t cache_cap = 128>
    struct pooling_base {
        static_assert((cache_cap & (cache_cap - 1)) == 0, "CacheSize must be power of two");

        static constexpr size_t align() noexcept {
            return alignof(element_t) > alignof(std::max_align_t)
                   ? alignof(element_t) : alignof(std::max_align_t);
        }

        static constexpr size_t alloc_size() noexcept {
            return (sizeof(element_t) + align() - 1) & ~(align() - 1);
        }

        struct cache_stack {
            void* ptrs[cache_cap];
            size_t top = 0;

            bool push(void* p) noexcept {
                LIKELY_IF(top < cache_cap) {
                    ptrs[top++] = p;
                    return true;
                }
                return false;
            }

            void* pop() noexcept {
                LIKELY_IF(top > 0) {
                    return ptrs[--top];
                }
                return nullptr;
            }
        };

        static cache_stack& cache() noexcept {
            static thread_local cache_stack c;
            return c;
        }

        static void* operator new (std::size_t n) {
            static_assert(std::is_final<element_t>::value, "the derived struct(class) must be tagged as final!");
            void* p = operator new(n, std::nothrow);
            UNLIKELY_IF(!p) {
#if FLUEX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
                throw std::bad_alloc();
#else
                std::abort();
#endif
            }
            return p;
        }

        static void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
            static_assert(std::is_final<element_t>::value, "the derived struct(class) must be tagged as final!");
            UNLIKELY_IF(n != sizeof(element_t)) return nullptr;
            if (void* p = cache().pop()) return p;
            return aligned_alloc(align(), alloc_size());
        }

        static void operator delete(void* p) noexcept {
            UNLIKELY_IF(!p) { return; }
            if (!cache().push(p)) {
                aligned_free(p);
            }
        }

        static void operator delete(void* p, const std::nothrow_t&) noexcept {
            operator delete(p);
        }

        static void operator delete(void* p, std::size_t) noexcept {
            operator delete(p);
        }

        static void* operator new[](std::size_t) = delete;
        static void operator delete[](void*) = delete;
    };
}

#endif //FLUX_FOUNDRY_POOLING_H