# Lifecycle and batching optimization experiments

These probes examine seven parts of the lifecycle/batching pass: flow continuation
storage and child ownership, controller allocation, executor draining, remote
returns, libuv submission, io_uring awaitables, and DPDK awaitables. They isolate
specific costs or compare explicit policies. Their results do not establish an
application-wide speedup.

The baseline is commit
[`a93d246981a49648949c489a1918f5ccb61ed15d`](https://github.com/OtakuNathan/flux_foundry/commit/a93d246981a49648949c489a1918f5ccb61ed15d).
Use that immutable revision when reproducing this pass, and retain the candidate
revision or working-tree diff with the run. `core` and `simple` compare matched
baseline/candidate builds. The new allocator, batching and terminal-only APIs
also have comparisons between policies within the candidate; those are not
automatically baseline-versus-candidate comparisons.

The measured report and decisions are in
[`results/2026-11-13-vm/report.md`](results/2026-11-13-vm/report.md).
Results from this VM are provisional: affinity does not remove host scheduling, shared-cache, frequency,
or virtualization effects. Inspect repeated raw observations and retest the
relevant workload on its deployment hardware before selecting a policy.

## Driver

Run from the repository root:

```sh
python3 bench/optimization_pass/run.py \
  --out /tmp/ff-optimization-pass \
  --baseline a93d246981a49648949c489a1918f5ccb61ed15d \
  --repeats 7 \
  --families core,controller,simple,identity,uv,uring,dpdk \
  --cpu 2
```

Choose a CPU permitted by the host's affinity mask; `2` is an example. Cross-thread
probes also need their actual owner/producer placement recorded. The driver
builds C++14 probes at `-O3`, runs measurements serially, and rotates comparison
order. Avoid other benchmark or compilation jobs during the measurement phase.
Optional `libuv`, `liburing`, and `libdpdk` dependencies are detected through
`pkg-config`; unavailable components must be reported as unavailable rather
than represented by a substitute backend.

Build and execution can be separated using the same output directory and
configuration:

```sh
python3 bench/optimization_pass/run.py \
  --out /tmp/ff-optimization-pass \
  --baseline a93d246981a49648949c489a1918f5ccb61ed15d \
  --families core,controller,simple,identity,uv,uring,dpdk --build-only

python3 bench/optimization_pass/run.py \
  --out /tmp/ff-optimization-pass \
  --baseline a93d246981a49648949c489a1918f5ccb61ed15d \
  --families core,controller,simple,identity,uv,uring,dpdk \
  --repeats 7 --cpu 2 --run-only
```

`--families` accepts a comma-separated subset of those seven names. The source
CLIs below also permit focused reruns. Keep compiler, flags, library versions,
workload dimensions, warmup, CPU placement and instrumentation mode matched
within each comparison. A change of those conditions creates a different
experiment.

## Experiment scopes

| Driver family | Source | Comparison and measured scope |
| --- | --- | --- |
| `core` | [flow_core.cpp](flow_core.cpp) | Continuation storage with a queued handoff; four-child fast aggregation launch bookkeeping. |
| `controller` | [controller.cpp](controller.cpp) | Ordinary controller allocation versus the opt-in pooled controller allocator, including runner construction, execution and release. |
| `simple` | [simple_drain.cpp](simple_drain.cpp), [existing scheduled probe](../queue_capacity/queue_capacity.cpp) | Prefilled successful-pop drain cost, plus a separate scheduled cross-thread observation. |
| `identity` | [identity_routes.cpp](identity_routes.cpp) | One versus four outgoing routes for remote frees to one or several live allocating owners. |
| `uv` | [uv_batch.cpp](uv_batch.cpp) | Individual versus accepted-prefix submissions; automatic versus forced compatibility fences. |
| `uring` | [io_fast.cpp](io_fast.cpp) | Ordinary versus terminal-only awaitables, with owning and borrowed fast runners, alongside raw/context/normal-runner controls. |
| `dpdk` | [dpdk_fast.cpp](dpdk_fast.cpp) | The same awaitable/runner distinctions on actual DPDK software `net_ring` loopback. |

### 1. Flow continuation storage and child ownership

```text
flow_core continuation|aggregate [ITERATIONS]
```

`continuation` uses an ordinary awaitable that completes during submission and
queues its continuation to a small local executor. Each operation constructs
an owning runner; a stable caller-owned controller excludes controller
allocation from the comparison. The receiver deliberately contains a
`std::shared_ptr`, giving the common 16-byte receiver shape on the measured
64-bit build. The probe includes awaitable lifecycle, ownership bookkeeping,
and the queued task; it is not a timing of a single allocation instruction.

Packing the runner data and adaptor into the existing C++14 `compressed_pair`
allows an empty adaptor to use empty-base storage. The common generated
continuation changes from 56 to 48 bytes on the checked 64-bit GCC layout and
fits the existing 48-byte callable SBO. The
[continuation contract](../../test/continuation_storage_contract.cpp) checks
the actual closure before erasure. Other receiver sizes, stateful adaptors and
ABIs can still require external callable storage. The active runner's owning
handles remain present through submission and immediate/concurrent completion.

`aggregate` launches four synchronous transform children through a fast
`when_all` node. It deliberately exposes child-launch overhead. Each launch
moves its per-invocation blueprint handle into an owning child runner; the
reusable factory retains its handles. The borrowed parent blueprint remains
alive throughout the loop. This does not measure concurrent fan-out or
recommend async aggregation for ordinary synchronous arithmetic. Pending
children, including fast `when_any` losers, retain their existing ownership
contracts, covered separately by the
[aggregation contract](../../test/aggregation_ownership_contract.cpp).

Both modes warm up before timing and validate completion counts and checksums.
`ns_per_op` is a loop average, not a request latency percentile.

### 2. Controller storage

```text
controller system|system-eager|pooled [ITERATIONS]
```

All three policies run the same synchronous blueprint with a fresh
cancellation-capable runner per iteration. The `system` lane uses the ordinary
receiver factory, which creates its controller at invocation. `system-eager`
uses the new allocator-tag factory with `aligned_malloc_allocator`; `pooled`
uses that same factory with `pooled_controller_allocator<>`. Compare
`system-eager` with `pooled` to hold eager construction constant and focus on
the storage policy. Compare `system` with `system-eager` to assess the factory
placement change separately. The measured interval includes construction,
invocation and destruction for all policies. This is a warmed,
same-thread storage comparison. It does not measure cancellation contention,
remote returns, or long-lived external cancellation handles.

The pool recycles the complete aligned control block only after its last owner
releases it. Retaining a cancellation handle can delay reuse; completing the
receiver alone is insufficient. Each new controller starts with fresh cancel
state. The opt-in pool trades cached dead storage and return-routing work for
potentially fewer backing allocations. See the
[controller storage contract](../../docs/controller_pool.md) and its
[tests](../../test/controller_pool_contract.cpp).

Allocation counts are a separate diagnostic build; see the instrumentation
section below. A reduction in calls to the aligned backing allocator must not
be described as eliminating controller construction or all allocation work.

### 3. Simple executor draining and scheduled handoff

```text
simple_drain [ROUNDS]
```

Each round prefills 1,024 tasks, requests shutdown, and times `run()` until it
drains. Queue filling and executor construction are outside the timed region.
The checksum validates every task. This focuses on the successful-pop path
after moving the backoff yield into the empty-queue branch. It includes task
invocation and release, but has no concurrent producer and cannot establish
cross-core throughput, wakeup latency or idle CPU behavior.

The driver's separate cross-thread cases (`scheduled` in its output) reuse
[the scheduled-arrival probe](../queue_capacity/README.md). Its CLI includes
`--capacity`, `--mode raw|fast|normal|all`, `--profile`, `--samples`, `--work`,
`--repeats`, `--producer-cpu`, and `--consumer-cpu`. Refer to that probe for the
supported profiles and metric definitions. Planned-arrival latency includes
producer lag; every scheduled operation is eventually attempted and checked.
The per-event timestamps and sample storage are part of this instrumented load
probe. Its tail metrics must remain separate from the uninstrumented drain
average. Raw, fast, and normal lanes also carry different flow ownership and
cancellation capabilities.

### 4. Remote-return routes

```text
identity_routes --routes 1|4 --owners 1..8 --blocks 1..256 \
  --rounds N --warmup N --order grouped|interleaved
```

Live owner threads allocate and fill 96-byte blocks. One releasing thread
validates their payloads, returns them in grouped or interleaved owner order,
and explicitly flushes pending remote returns after each round. Only the
deallocation/flush loop is timed. Allocation, payload validation, barriers and
thread teardown are outside it; owners park during the release phase.

Compare both route counts with identical owner count, order and block count.
Single-owner and grouped returns are useful controls for interleaved returns;
more live owners than routes expose eviction. The reported median and p99 are
percentiles of **per-round average nanoseconds per released block**, not
percentiles of individual release or request latency.

One route remains the default. Four routes can preserve batches across
interleaved allocation origins, at the cost of a bounded search and more
retained dead blocks. With batch size 16, an idle sender can retain up to
`Routes * 15` unflushed blocks per specialization: 15 versus 60 here. Explicit
flushes in this probe bound the delay; an application without those boundaries
can retain partial batches longer. The allocation identity and its generation
still govern remote returns and stale-owner fallback. See
[identity-pool contracts](../../docs/identity_pool.md).

### 5. libuv batch submission and fences

```text
uv_batch single|batch REQUESTS_PER_PRODUCER BATCH PRODUCERS CAPACITY BUDGET \
  automatic|fence [OWNER_CPU FIRST_PRODUCER_CPU]
```

Both submission modes construct the same task groups. Producers saturate the
bounded queue while its owner drains through the libuv loop. The measured
interval runs from producer release to completed tasks and finished submission;
thread/loop construction and final close are outside it. The probe verifies
every producer's sequence and final count. `ns_per_task` is aggregate elapsed
time divided by completed tasks. It is not per-task latency, an I/O fairness
measurement, or a network throughput measurement.

Batch submission moves an accepted prefix, leaves its suffix intact, and uses
one admission ticket and notification for that prefix. It always queues,
including on the owner; it does not adopt single-submission owner-inline
semantics. Concurrent batches may interleave. The public executor and task
array must remain valid through their submission calls. This probe retries
until every task is accepted, so its timings include the selected backpressure
and yield behavior.

The `retries` columns are **not comparable event counts across modes**. In
`single`, one increment means one rejected task submission. In `batch`, one
increment means a call left an unfinished group, even if that call accepted a
nonempty prefix. Do not interpret their ratio as a queue rejection-rate or
contention reduction.

`automatic` chooses the compatibility fence from the linked libuv runtime;
`fence` always requests it. With a runtime older than 1.53.0 both execute the
compatibility fence, so that host cannot measure its removal. Keep the reported
runtime version with the results. API and shutdown details are in the
[libuv executor documentation](../../docs/uv_executor.md).

### 6. io_uring terminal-only awaitables

```text
io_fast kernel|staged MODE COUNT BATCH CPU
MODE = raw|context|normal|ordinary-owning|ordinary-view|terminal-owning|terminal-view
```

`BATCH` must be a power of two no greater than 32,768; `COUNT` must be a positive
multiple of it. Every operation is a NOP carrying a checked identifier.

`staged` prepares userspace SQEs and routes their tokens through the production
attachment, but the harness supplies completions after each batch. It performs
no kernel I/O. `kernel` initializes a real ring, submits NOPs and waits for their
CQEs. Even that path does not measure file, storage or network I/O. An io_uring
initialization denial or unsupported kernel is a skipped kernel experiment
(exit 77), not a staged result that can be relabeled as kernel throughput.

`normal` uses an ordinary awaitable and a cancellation-capable runner. The
`ordinary-*` and `terminal-*` modes use fast runners; `owning` retains a blueprint
handle, while `view` borrows a blueprint kept alive by the harness. Compare
ordinary-owning against terminal-owning, or ordinary-view against terminal-view,
to hold that ownership choice constant. Raw and context-only modes provide
lower-level references with fewer lifecycle features.

The terminal-only node omits early controller-driven completion and the
ordinary adapter's separate backend-reference pair. It still owns its operation
until the accepted request's original terminal CQE. The host must submit and
drain accepted requests through close or a failed/short flush. Borrowed
blueprints must outlive deferred continuations. This is an explicit semantic
choice, not a replacement for callers that need early logical cancellation.

### 7. DPDK terminal-only awaitables

```text
dpdk_fast BURSTS REPEATS CPU
```

The executable runs `raw`, `batch`, `context`, `normal`, `ordinary-owning`,
`ordinary-view`, `terminal-owning`, and `terminal-view` modes with burst sizes
1 and 32, rotating mode order between repetitions. `batch` is the RAII packet
batch control. Each mode validates 64-byte packet contents and counts; final
drain checks ring and mbuf reclamation.

This uses actual DPDK EAL and the software `net_ring` PMD, with a hot set of mbufs
circulating through same-core RX/TX loopback. There is no physical NIC, DMA,
wire traffic, PCIe or external packet arrival. `ns_per_packet` is burst-average
cost divided by burst width, not packet latency. Results do not establish NIC
line rate or a production polling core's capacity.

Owning/view and ordinary/terminal comparisons have the same distinction as the
io_uring modes. A terminal-only RX waiter completes through its accepted backend
callback; controller cancellation alone cannot complete it early. An empty RX
remains pending. Attachment close still completes the waiter on the next poll,
so the host must continue polling to drain. Packet batches retain their existing
ownership and partial-TX contract, and their native mempool must outlive them.
The probe's successful full loopback TX does not exercise partial-TX pressure.
See [I/O attachment contracts](../../docs/io_attachments.md) for both adapters.

## Separate diagnostics from timings

The timing builds must omit the following diagnostic macros. Build and run
their instrumented counterparts separately, with the same workload parameters,
to explain a timing observation without changing its measured path.

**Identity batches.** `FF_IDENTITY_ROUTES_TRACE` enables an untimed histogram
through `FLUX_FOUNDRY_IDENTITY_POOL_TRACE_BATCH`. It records batch lengths,
publication/adoption counts and checked block totals, and deliberately emits no
timings. The trace validates whether route changes actually changed batching.
It does not measure peak retained memory or end-to-end allocator latency.

```sh
c++ -std=c++14 -O3 -DNDEBUG -pthread -I. \
  -DFF_IDENTITY_ROUTES_TRACE=1 \
  bench/optimization_pass/identity_routes.cpp -o /tmp/ff-identity-trace
/tmp/ff-identity-trace --routes 4 --owners 4 --blocks 256 \
  --rounds 20 --warmup 10 --order interleaved
```

**Controller allocations.** On a toolchain supporting the GNU-style linker
wrapper, `FF_CONTROLLER_TRACE_ALLOC` with `--wrap=posix_memalign` counts calls
to that backing allocation function in the post-warmup loop. It is not a
counter of pool acquisitions, constructors, all heap calls, frees, or retained
bytes. The diagnostic output leaves `ns_per_op` empty; timing builds leave
`aligned_allocations` empty because they did not collect it. An empty field
means unmeasured, not zero.

```sh
c++ -std=c++14 -O3 -DNDEBUG -pthread -I. \
  -DFF_CONTROLLER_TRACE_ALLOC=1 \
  bench/optimization_pass/controller.cpp -Wl,--wrap=posix_memalign \
  -o /tmp/ff-controller-trace
/tmp/ff-controller-trace system 1000000
/tmp/ff-controller-trace system-eager 1000000
/tmp/ff-controller-trace pooled 1000000
```

Keep diagnostic rows, correctness/sanitizer runs, uninstrumented timing rows,
and scheduled per-event latency observations distinguishable in the report.
Checksums and lifecycle contracts are prerequisites for interpreting a result;
benchmark success alone does not validate cancellation, rejection, stale-owner,
or concurrent teardown behavior. Report unsupported cases and variability
alongside the measurements, and retain raw rows rather than only a best run or
one aggregate percentage.
