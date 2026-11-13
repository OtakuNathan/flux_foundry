# Continuation storage and child blueprint ownership

The lifecycle/batching pass keeps the normal and fast runner contracts intact.
It reduces two costs in the flow implementation.

## Compress empty continuation adaptors

Async continuations now capture the owned runner data and adaptor inside the
existing C++14 `compressed_pair`. Empty adaptors such as `identity` use empty-base
storage instead of occupying a lambda member plus alignment padding. The same
packed state moves into a queued continuation task when a non-inline executor
is selected. Stateful and mutable adaptors retain their concrete type and state.

On a 64-bit GCC build, a normal cancel-aware continuation with a pointer executor
and a 16-byte receiver changes from 56 to 48 bytes. It fits the existing 48-byte
`callable_wrapper` SBO buffer, avoiding the separate pooled callable payload.
This is a layout result for that capture shape; other receivers, adaptors,
platforms and alignment requirements can still require external storage. The
SBO capacity and wrapper object sizes are unchanged.

The active runner's blueprint/receiver and cancellation-controller ownership
are still copied at submission. Moving those owners away before submission
returns would require a different lifetime protocol for immediate completion,
concurrent completion, and failed submission. This change does not do that.

## Transfer per-operation child blueprint handles

Each `when_all`/`when_any` invocation first obtains child blueprint handles from
its reusable factory. A child launch now moves its per-invocation handle into
the owning child runner, in both normal and fast aggregators. The reusable
factory keeps its own handles. This removes one retain/release pair per launched
child while retaining ownership throughout that child's execution.

The change does not turn child runners into borrowed views. Fast `when_any`
losers continue to own their blueprint after the winner completes and the
parent blueprint is released. Normal cancellation still follows the existing
child-controller and backend-reference protocol.

## Validation and measurement

- `test/continuation_storage_contract.cpp` observes the actual generated
  continuation before type erasure. It checks the common SBO boundary, identity
  and stateful mutable adaptors, normal/fast runners and awaitables, inline,
  worker-before-return, delayed and rejected submission, cancellation, and
  destruction of blueprint captures after public handles are dropped.
- `test/aggregation_ownership_contract.cpp` checks child ownership counts,
  asynchronous capture lifetime, a fast winner with a pending loser, and repeated
  invocations of a shared factory. Existing pipeline lifetime tests cover nested
  joins, rejection, cancellation and completion order.
- `bench/optimization_pass/flow_core.cpp` provides targeted `continuation` and
  `aggregate` probes. The first uses a stable caller-owned controller to isolate
  continuation storage from controller allocation. The second deliberately
  uses four synchronous child calculations inside a fast aggregator to expose
  launch bookkeeping; it is not a recommendation to use async aggregation for
  ordinary synchronous arithmetic.

Build matched binaries against the baseline and candidate headers, use the same
compiler and flags, and alternate process order on the same pinned CPU. Report
raw timing distributions and validated checksums. These local mechanism probes
do not establish production tail latency, network throughput or a universal
improvement. Allocation counters must distinguish pooled object acquisition
from a system allocation.
