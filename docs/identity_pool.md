# Awaitable identity pool

`FLUX_FOUNDRY_AWAITABLE_IDENTITY_POOL=1` selects a headerless, identity-routed
allocator for both ordinary and fast awaitables. It is **on by default**.
The awaitable/runner contract choices remain independent; cancellation,
submission references and terminal completion rules do not change.

```cmake
target_compile_definitions(your_target PRIVATE
    FLUX_FOUNDRY_AWAITABLE_IDENTITY_POOL=0) # Optional legacy policy
```

Keep this macro, `FLUX_FOUNDRY_AWAITABLE_POOL_SLOT_COUNT`, and
`FLUX_FOUNDRY_AWAITABLE_POOL_ROUTE_COUNT` consistent across
every translation unit/library exchanging FF types. Enabling the policy adds
one `size_t` identity member to the awaitable base, and can change object
size/alignment padding and its backing pool size class. There is no prefix
header attached to the raw allocation. Generic `flux_foundry_allocator`,
`pooling_base`, blueprint allocation and task type erasure remain unchanged.

## Ownership and batching

Each allocator specialization has a stable table of 64 registration slots.
Each participating thread has a TLS local cache, bounded MPSC inbox, and a
fixed set of outgoing batch lanes. The default route count is **one**, retaining
the existing allocation policy. The identity names a slot plus its generation, not a TLS
address or OS thread ID. The current generation, close bit and publisher
count share a single atomic CAS domain.

- Allocation pops the TLS cache. On a miss, the owner detaches its inbox with
  one exchange, fills its local cache, and returns excess blocks to backing
  storage. If still empty, it uses FF's shared size-class pool or heap fallback.
- Same-owner deallocation goes directly to the TLS cache, without registry or
  shared atomic operations while there is room.
- Remote deallocation builds an intrusive list in **already destroyed**
  object storage. Sixteen blocks for the same identity are published together.
  With one lane, switching destinations flushes the previous partial batch.
  With multiple lanes, the sender first finds an exact identity match or an
  unused lane. When every lane is occupied, it flushes one lane using a bounded
  round-robin eviction policy. A full batch flushes only its own lane.
- Publication acquires one registry ticket for the batch, reserves bounded
  inbox capacity, CAS-pushes the whole list, and releases the ticket. It never
  dereferences the previous inbox head. Only the owner removes inbox lists.
- A closed/stale identity, exhausted admission count or full inbox transfers
  the free blocks into the sender's compatible local cache; excess blocks go
  to backing storage. A subsequent allocation records its new owner's identity.

An idle live sender can retain up to `Routes * 15` unflushed blocks per allocator
specialization: 15 with the default single lane, or 60 with four lanes. It holds
**no publisher ticket** while accumulating any batch. For a processing boundary
that must release every partial batch, call:

```cpp
// On the thread that performed the remote releases:
MyAwaitable::flush_remote_returns();
```

The call flushes every lane, including lanes whose owners have closed or whose
registry slots have changed generation; those blocks are adopted locally.
TLS exit also flushes every lane automatically. This policy has no timer, executor dependency,
or background collection thread. Owner inbox capacity equals its local cache
capacity, which defaults to the awaitable cache setting, 256. Each TLS pool
can retain up to 256 local blocks, 256 incoming blocks (including publication
reservations), and `Routes * 15` outgoing blocks. This is a limit, not eager preallocation.
The legacy `pooling_base` currently does not forward the cache setting to its
allocator, whose actual capacity is 512. Policy-only comparisons use 512
on both builds; capacity screens explicitly record the candidate's capacity.

### Interleaved allocation origins

A consumer releasing interleaved completions from several allocating threads
can repeatedly change destination. One outgoing lane then publishes small
partial batches even when `Batch` is 16. A fixed four-lane policy can be selected
for an application that has measured this pattern:

```cmake
target_compile_definitions(your_target PRIVATE
    FLUX_FOUNDRY_AWAITABLE_POOL_ROUTE_COUNT=4)
```

This is a workload policy, not a universal throughput setting. Matching an
origin costs a bounded linear search through the configured lanes; more live
origins than lanes can still cause eviction on every release. Same-owner
deallocation and local allocation do not perform that search. Four lanes add
45 to the maximum number of pending dead blocks per TLS allocator specialization
and add fixed per-lane bookkeeping. A producer waiting for a small partial batch
can also receive its storage later unless the sender explicitly flushes at a
processing boundary. Defaults remain one lane until an application's
throughput, tails, and retained-memory measurements justify another choice.

## Thread exit and stale identities

TLS destruction disables cache resurrection, flushes outgoing blocks, closes
the registration, and waits for **already admitted publishers**. It then
drains the inbox and local cache before releasing the registration slot.
It does not wait for live awaitables or batches pending on other threads.
No remote publisher may touch the TLS endpoint without a ticket.

This wait is confined to teardown, but a descheduled/stalled admitted
publisher can delay thread exit: teardown is not wait-free. Once closed, new
publishers adopt locally. Reuse increments the generation; at generation
exhaustion a slot is permanently retired instead of wrapping. If all slots
are occupied/retired, identity zero selects safe local adoption. Neither
registry exhaustion nor an overflowing inbox blocks allocation or drops memory.

Live blocks remain backed by the shared pool / heap when their allocating
thread exits. Late TLS destructors bypass the retired cache and use backing
storage directly. The table itself has process-static storage; it does not
grow with thread churn. It is a registration/admission table, not a general
hazard-pointer implementation.

## Object lifetime contract

The FF final-release path saves the identity while the awaitable is alive,
runs its destructor, then returns the storage using the saved identity.
Ordinary explicit `delete` and constructor-failure cleanup cannot read the
identity after destruction, so they safely adopt on the deleting thread.

Identity-pooled awaitables must be final, have accessible nonthrowing destructors,
and use the inherited allocation/deallocation functions. Compile-time checks
reject overrides that would make the explicit final-release path mismatch
the storage allocator. Array allocation is disabled.

The low-level `identity_pool_allocator<Size, Align, Cache, Batch, Slots, Remote, Routes>`
is available separately (its standalone cache default is 512; awaitables
explicitly pass their configured capacity). `Routes` is appended with a default
of one, so existing six-argument specializations keep their policy; it must be
nonzero and no greater than `Slots`. `identity_pooling_base<T, Cache, Routes>`
uses the awaitable route-count setting for its default `Routes`. Call `alloc()`, save `current_identity()` on the same
thread, and later call `dealloc(saved_identity, pointer)` using the **same
specialization** after the payload's lifetime ends. Size and alignment must
accommodate a free-list pointer. Counters and pointers must be always lock-free;
invalid sizes, capacities and alignments are rejected by `static_assert`.

## Validation

`identity_pool_contract` covers origin reuse, partial batches, multiple routes,
bounded-inbox pressure, concurrent owner/publisher exit, stale generations,
registry exhaustion, adoption, late TLS destruction, constructor failure,
explicit delete, heap fallback and over-alignment. It checks complete backing
pool reclamation after thread exit. The existing awaitable lifecycle and
pipeline lifecycle suites also run with this policy, with and without exceptions.
Compile-failure tests verify intended diagnostic messages.

The multi-route cases additionally cover interleaved two/four/eight-owner
returns, more owners than lanes, explicit flushing while the sender stays alive,
all-lane TLS flushing, full inbox adoption, two stale lanes with reused registry
slots, and four-owner/four-sender closure races. They also run selected cases
with shared backing storage exhausted to force heap fallback.

`test/model/IdentityPool.tla` models admission, delayed publication, bounded
inbox reservation, owner drain, close, adoption and slot reuse. It checks
ownership and lifetime invariants for individual batches, not the outgoing-lane
replacement policy, the full C++ memory model or liveness.
Performance should be evaluated for the intended placement and load; the
legacy policy remains available for workload-specific comparison.

The [capacity and latency study](../bench/io_adapters/results/2026-11-13/identity-pool/README.md)
records the choice of 256 cached blocks and batches of 16, including throughput,
actual-submission latency, single-core controls and observed variability.

### Focused route benchmark

`bench/optimization_pass/identity_routes.cpp` supplies a reproducible allocator
probe with one releasing thread and one to eight live allocating owners. It
compares grouped and round-robin release order with one or four outgoing lanes.
Allocation, payload verification, round barriers and thread teardown stay
outside the measured deallocation/explicit-flush loop. Owners park between
rounds. The output includes checked block counts, a checksum, and median/p99
of **per-round average ns per released block**; that p99 is not a per-request
latency percentile and the probe is not an executor throughput measurement.

Build the timing and histogram binaries separately:

```sh
c++ -std=c++14 -O3 -DNDEBUG -pthread -I. \
    bench/optimization_pass/identity_routes.cpp -o /tmp/ff-identity-routes
c++ -std=c++14 -O3 -DNDEBUG -pthread -I. -DFF_IDENTITY_ROUTES_TRACE=1 \
    bench/optimization_pass/identity_routes.cpp -o /tmp/ff-identity-histogram
/tmp/ff-identity-routes --routes 4 --owners 4 --order interleaved --rounds 200
/tmp/ff-identity-histogram --routes 4 --owners 4 --order interleaved --rounds 20
```

Run both route counts with identical arguments, alternate their order across
independent processes, and include grouped/single-owner controls. The diagnostic
build defines `FLUX_FOUNDRY_IDENTITY_POOL_TRACE_BATCH(origin, count, published)`
and reports actual batch lengths, publication/adoption counts and total blocks;
it deliberately emits no timings. Normal builds contain no tracing state or
callback. A custom hook must be `noexcept`, must not re-enter the allocator, and
must be configured consistently across translation units sharing a specialization.
The hook observes copied scalar metadata after publication or adoption, never
the returned object storage or an unprotected destination cache.
