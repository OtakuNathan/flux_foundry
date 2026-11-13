#include "memory/identity_pool.h"
#ifndef FF_IDENTITY_CASE
#define FF_IDENTITY_CASE 0
#endif
using namespace flux_foundry;
#if FF_IDENTITY_CASE == 1
using pool = identity_pool_allocator<1, 8>;
#elif FF_IDENTITY_CASE == 2
using pool = identity_pool_allocator<96, 3>;
#elif FF_IDENTITY_CASE == 3
using pool = identity_pool_allocator<96, 8, 0>;
#elif FF_IDENTITY_CASE == 4
using pool = identity_pool_allocator<96, 8, 32, 0>;
#elif FF_IDENTITY_CASE == 5
using pool = identity_pool_allocator<96, 8, 32, 16, 256>;
#elif FF_IDENTITY_CASE == 6
using pool = identity_pool_allocator<96, 8, 32, 16, 64, 8>;
#elif FF_IDENTITY_CASE == 7
using pool = identity_pool_allocator<std::numeric_limits<size_t>::max(), 8>;
#else
using pool = identity_pool_allocator<96, 8>;
#endif
struct object final : identity_pooling_base<object> {
    char data[96];
#if FF_IDENTITY_CASE == 8
    using identity_pooling_base<object>::operator new;
    static void* operator new(size_t n) { return std::malloc(n); }
#elif FF_IDENTITY_CASE == 9
    static void operator delete(void* p) noexcept { std::free(p); }
#elif FF_IDENTITY_CASE == 10
    using identity_pooling_base<object>::operator new;
    static void* operator new(size_t n, const std::nothrow_t&) noexcept { return std::malloc(n); }
#endif
};
int main() {
    auto p = pool::alloc(); pool::dealloc(pool::current_identity(), p);
    object::destroy_pooled(new object);
}
