#ifndef FLUX_FOUNDRY_FLOW_CONTROLLER_POOL_H
#define FLUX_FOUNDRY_FLOW_CONTROLLER_POOL_H

#include <memory>
#include <type_traits>
#include <utility>

#include "flow_runner.h"
#include "../memory/identity_pool.h"

namespace flux_foundry {

// Optional storage policy for a controller's complete lite_ptr control block.
// The allocator copy in that block records its allocation origin. Storage only
// returns to the pool after the LAST controller owner releases it, including
// externally retained cancellation handles. Completion alone never recycles it.
template <size_t Cache = 256, size_t Routes = FLUX_FOUNDRY_AWAITABLE_POOL_ROUTE_COUNT>
class pooled_controller_allocator {
    static_assert(Cache >= 16, "controller pool capacity must hold a 16-block return batch");
    static_assert(Routes > 0, "controller pool requires at least one outgoing route");
    size_t origin_ = 0;
public:
    void* allocate(size_t alignment, size_t bytes) noexcept;
    void deallocate(void* p) const noexcept;
    static void flush_remote_returns() noexcept;
};

namespace detail {
template <size_t Cache, size_t Routes>
struct controller_pool_storage {
    using allocator = pooled_controller_allocator<Cache, Routes>;
    using pointer = lite_ptr<flow_controller, default_deleter<flow_controller>,
                             alignof(flow_controller), allocator>;
    using pool = identity_pool_allocator<pointer::required_allocation_size(),
        pointer::required_allocation_align(), Cache, 16, 64, Cache, Routes>;
};
}

template <size_t Cache, size_t Routes>
void* pooled_controller_allocator<Cache, Routes>::allocate(size_t alignment, size_t bytes) noexcept {
    using storage = detail::controller_pool_storage<Cache, Routes>;
    using pointer = typename storage::pointer;
    using pool = typename storage::pool;
    // This policy serves this exact control-block layout, not arbitrary payloads.
    if (alignment != pointer::required_allocation_align() ||
        bytes != pointer::required_allocation_size()) return nullptr;
    origin_ = pool::current_identity();
    return pool::alloc();
}

template <size_t Cache, size_t Routes>
void pooled_controller_allocator<Cache, Routes>::deallocate(void* p) const noexcept {
    detail::controller_pool_storage<Cache, Routes>::pool::dealloc(origin_, p);
}

template <size_t Cache, size_t Routes>
void pooled_controller_allocator<Cache, Routes>::flush_remote_returns() noexcept {
    detail::controller_pool_storage<Cache, Routes>::pool::flush_remote_returns();
}

// Explicit controller allocation policy. Unlike the ordinary receiver overload,
// this factory eagerly constructs the controller using the supplied allocator.
// Blueprint ownership and cancellation behavior are otherwise unchanged.
template <typename Alloc, typename BP, typename Receiver>
auto make_runner(std::allocator_arg_t, Alloc&& allocator, lite_ptr<BP> bp, Receiver receiver) {
    auto controller = make_lite_ptr_with_allocator<flow_controller>(std::forward<Alloc>(allocator));
    using controller_ptr = decltype(controller);
    return flow_runner<BP, Receiver, controller_ptr>(
        std::move(bp), std::move(controller), std::move(receiver));
}

template <typename Alloc, typename BP>
auto make_runner(std::allocator_arg_t tag, Alloc&& allocator, lite_ptr<BP> bp) {
    return make_runner(tag, std::forward<Alloc>(allocator), std::move(bp),
                       stub_receiver<typename BP::O_t>{});
}

}
#endif
