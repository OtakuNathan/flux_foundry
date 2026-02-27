#ifndef FLUX_FOUNDRY_POOLING_H
#define FLUX_FOUNDRY_POOLING_H

#include <new>
#include <cstdint>
#include <stdexcept>

#include "aligned_alloc.h"
#include "utility/static_list.h"
#include "static_mem_pool.h"
#include "base/traits.h"

namespace flux_foundry {
    namespace detail {
        constexpr size_t alloc_size(size_t size, size_t align) noexcept {
            return (size + align - 1) & ~(align - 1);
        }

        constexpr size_t flux_foundry_default_cache_cap = 512;
        constexpr static size_t pool_max_block_size = 1024;
        constexpr static size_t max_block_count = 256;
        using pool_t = static_mem_pool<max_block_count, pool_max_block_size>;

        inline pool_t& get_pool() noexcept {
            static pool_t pool;
            return pool;
        }
    }

    template <size_t size, size_t align, size_t cache_cap = detail::flux_foundry_default_cache_cap,
            bool = (align <= alignof(std::max_align_t))>
    struct flux_foundry_allocator {
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

            ~cache_stack() noexcept {
                while (top > 0) {
                    auto p = ptrs[--top];
                    auto& pool = detail::get_pool();
                    if (pool.belong_to(p)) {
                        pool.deallocate(p);
                    } else {
                        free(p);
                    }
                }
            }
        };

        static cache_stack& cache() noexcept {
            static thread_local cache_stack c;
            return c;
        }

        void* alloc() noexcept {
            constexpr size_t sz = detail::alloc_size(size, align);

            void* p = nullptr;
            if ((p = cache().pop())) {
                return p;
            }

            LIKELY_IF((p = detail::get_pool().allocate(sz))) {
                return p;
            }

            return malloc(sz);
        }

        void dealloc(void* p) noexcept {
            UNLIKELY_IF(!p) {
                return;
            }

            LIKELY_IF (cache().push(p)) {
                return;
            }

            auto& pool = detail::get_pool();
            LIKELY_IF (pool.belong_to(p)) {
                pool.deallocate(p);
            } else {
                free(p);
            }
        }
    };

    template <size_t size, size_t align, size_t cache_cap>
    struct flux_foundry_allocator <size, align, cache_cap, false> {
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

            ~cache_stack() noexcept {
                while (top > 0) {
                    aligned_free(ptrs[--top]);
                }
            }
        };

        static cache_stack& cache() noexcept {
            static thread_local cache_stack c;
            return c;
        }

        void* alloc() noexcept {
            void* p = nullptr;
            LIKELY_IF ((p = cache().pop())) {
                return p;
            }

            return aligned_alloc(align, detail::alloc_size(size, align));
        }

        void dealloc(void* p) noexcept {
            UNLIKELY_IF(!p) {
                return;
            }

            LIKELY_IF (cache().push(p)) {
                return;
            }

            aligned_free(p);
        }
    };

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

        static void* operator new (std::size_t n) {
            static_assert(std::is_final<element_t>::value, "the derived struct(class) must be tagged as final!");
            void* p = operator new(n, std::nothrow);
            UNLIKELY_IF(!p) {
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
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
            return flux_foundry_allocator<sizeof(element_t), alignof(element_t)>().alloc();
        }

        static void operator delete(void* p) noexcept {
            flux_foundry_allocator<sizeof(element_t), alignof(element_t)>().dealloc(p);
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
