//
// Created by Nathan on 08/01/2026.
//

#ifndef LF_TEST_ALIGNED_ALLOC_H
#define LF_TEST_ALIGNED_ALLOC_H

#include <cstddef>
#include <cstdlib>
#include "../base/traits.h"
#ifdef _WIN32
#include <malloc.h>
#endif

namespace flux_foundry {
    inline void* aligned_alloc(size_t align, size_t size) noexcept {
        assert((align & (align - 1)) == 0 && "align must be power of 2");
        assert(align >= alignof(nullptr_t) && "align must be >= alignof(void*)");
#ifndef _WIN32
        void* _p = nullptr;
        UNLIKELY_IF (posix_memalign(&_p, align, size) != 0) {
            return nullptr;
        }
        return _p;
#else
        return _aligned_malloc(size, align);
#endif
    }

    inline void aligned_free(void* p) noexcept {
        UNLIKELY_IF (!p) {
            return;
        }
#ifndef _WIN32
        free(p);
#else
        _aligned_free(p);
#endif
    }

    struct aligned_malloc_allocator {
        void* allocate(size_t align, size_t size) const noexcept {
            return aligned_alloc(align, size);
        }

        void deallocate(void* p) const noexcept {
            aligned_free(p);
        }
    };
}

#endif //LF_TEST_ALIGNED_ALLOC_H
