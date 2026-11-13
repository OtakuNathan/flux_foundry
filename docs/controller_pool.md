# Optional controller storage policy

Cancellation-capable runners can choose a control-block allocator explicitly:

```cpp
#include "flow/flow.h"

auto runner = flux_foundry::make_runner(
    std::allocator_arg,
    flux_foundry::pooled_controller_allocator<>{},
    blueprint_handle,
    receiver);
runner(input);
auto cancellation = runner.get_controller();
```

The existing `make_runner` overloads keep their allocation behavior. The
allocator-tag overload eagerly creates a controller. It also accepts an
application allocator with the `lite_ptr` allocator interface:
`allocate(alignment, bytes)` and `deallocate(pointer)`. Its allocator copy is
stored in the control block, so any referenced allocator resource must outlive
all controller handles. Allocation failure follows `lite_ptr`: exceptions are
reported when enabled; no-exception construction can produce an empty handle,
which should be checked with `runner.get_controller()` before starting work.

`pooled_controller_allocator<Cache, Routes>` is an optional exact-layout policy
for `lite_ptr<flow_controller, ..., pooled_controller_allocator<...>>`. It is
not a generic allocator for other payloads. Its identity-routed pool preserves
the alignment of the complete control block and returns freed storage to its
allocating thread when that owner remains available. Closed, stale, full, or
unregistered destinations retain the identity pool's fallback/adoption behavior.
The default local/incoming capacities are 256 each; the batch size is 16 and
`Cache` must therefore be at least 16. `Routes` defaults to
`FLUX_FOUNDRY_AWAITABLE_POOL_ROUTE_COUNT` (one unless explicitly configured).
See [identity pool](identity_pool.md) for bounded retention and partial batches.

The last owning controller handle triggers destruction and recycling. A
receiver finishing does **not** make that storage reusable if an external
cancellation handle still owns it. Each newly constructed controller starts
with fresh cancellation state; late cancellation of an older operation cannot
affect a newer one. The runner, awaitable, and handler-detachment protocols are
unchanged.

Call `pooled_controller_allocator<...>::flush_remote_returns()` at an optional
processing boundary to publish partial batches. Thread exit also flushes them.
This policy is opt-in because retained memory and cross-thread locality depend
on the workload. It does not change fast runners, which have no controller.
