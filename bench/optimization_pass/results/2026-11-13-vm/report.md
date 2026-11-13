# Lifecycle and batching pass: measured results and decisions

All seven review proposals were implemented and exercised. The useful outcomes
are reduced lifecycle bookkeeping, a cheaper successful executor drain path,
and explicit policies for workloads that can benefit from batching or terminal
completion. These component results cannot be added together into an FF-wide
speedup.

## Disposition

| Proposal | Result | Decision |
| --- | --- | --- |
| Compress continuation data and empty adaptor | The checked outer closure fits the existing 48-byte SBO; one external callable-pool round trip disappears. Timing remains inconclusive. | Keep in the ordinary implementation; make no stable speedup claim. |
| Move per-operation child blueprint ownership | One retain/release pair removed per launched child; pending children retain ownership. The four-child microprobe has a favorable median. | Keep in all four async aggregator launch paths. |
| Cache several remote-return origins | Four routes help two/four interleaved origins and regress eight interleaved origins. | Keep one route by default; expose four as an explicit choice. |
| Advance simple-executor backoff only while idle | Prefilled drain cost consistently improves. Scheduled tails remain noisy. | Keep the successful-pop change enabled. |
| Batch UV admission/wakes and remove a redundant compatibility fence | Explicit batch16 improves measured drain throughput. Independent fence benefit is unresolved. | Add batch API; preserve single-submit behavior and default drain budget. Use the runtime ordering guarantee, with `always_fence` available. |
| Pool normal-runner controller storage | Warm same-thread reuse reduces aligned allocation calls and measured cost. | Opt-in allocator-tag factory; last external owner still controls recycling. |
| Expose terminal-only io_uring/DPDK awaitables | Lifecycle tests pass. Staged io_uring batch32 has a positive timing signal; native backend measurements are unavailable here. | Public opt-in fast adapters; existing cancellable adapters remain unchanged. |

## Environment and method

Baseline: [`a93d246981a49648949c489a1918f5ccb61ed15d`](https://github.com/OtakuNathan/flux_foundry/commit/a93d246981a49648949c489a1918f5ccb61ed15d).
Candidate source hashes and exact build/run commands are in
[provenance.json](provenance.json). The source belongs to the same change as this
report, rather than to a subsequently floating `main` revision.

- Linux x86-64 VM, reporting AMD EPYC 7763; nine permitted logical CPUs, 0–8.
  Sysfs CPU topology is unavailable. Pinning does not provide an isolated core
  or control host scheduling, frequency, caches or virtualization.
- GCC 14.2, C++14, `-O3 -DNDEBUG -pthread` for timing. The existing scheduled
  probe additionally uses strict no-exception/no-RTTI flags in both builds.
- libuv 1.53.0, liburing 2.15, DPDK 25.11.0, all built from tagged upstream
  sources. No physical NIC or external service was involved.
- Five independent processes per primary variant, rotating variant order.
  All compilation finished before measurements; processes ran serially.
  Single-thread flow/controller/drain measurements used CPU 0. UV and scheduled
  probes placed their consumer/producer threads on distinct permitted CPUs.
  Identity owners were unpinned and parked during timed returns.
- The primary driver launched 356 processes: 335 timing processes, 19 separate
  instrumentation processes and two unavailable-native-backend probes. A
  subsequent 14-process continuation check addressed an unfavorable initial
  median; both series are retained.
- Tables use the **median of process measurements**. Bracketed ranges are the
  complete process minimum–maximum, not confidence intervals. Percentage
  changes compare medians, not the median of paired percentage changes.

Data: [raw observations](raw.csv), [primary summary](summary.csv),
[scheduled throughput/tail summary](scheduled-summary.csv),
[longer continuation check](continuation-confirmation.csv), and
[validation log](validation.txt). No outlier was discarded.

## 1. Continuation storage: verified mechanism, unresolved timing

The normal cancel-aware, non-inline continuation with the probe's 16-byte
receiver changes from 56 bytes to 48 bytes and fits the existing callable SBO.
The active runner still owns its blueprint/receiver/controller while submission
returns; the change packs the captured state, without moving away those live
owners. Stateful/mutable adaptors remain supported.

Inspection of the two GCC binaries also shows:

| Storage in this probe | Before | After |
| --- | --- | --- |
| Awaitable allocation size/alignment | 112 / 16 bytes | 112 / 16 bytes |
| Outer callback | 56 bytes, external storage | 48 bytes, SBO |
| Queued task payload | 64 bytes, external storage | 56 bytes, still external |

Thus **one** warmed callable-pool acquisition/return is removed per operation.
The entire continuation chain does not become allocation-free. The source
change introduces no additional blueprint/receiver retain pair. The generated
function's overall code layout also changes, so storage improvements alone do
not establish latency improvements on every compiler or workload.

| Series | Before ns/op, median [range] | After ns/op, median [range] |
| --- | ---: | ---: |
| Initial: 1 million operations, 5 processes | 148.399 [115.978–189.340] | 162.336 [127.342–253.020] |
| Follow-up: 10 million operations, 7 processes | 112.227 [101.150–136.355] | 110.893 [90.897–145.826] |

The initial median is 9.4% worse; the longer median is 1.2% better, with four
faster and three slower matched repeats. This environment establishes neither
a repeatable speedup nor a repeatable slowdown. The change is retained for its
verified storage reduction and unchanged ownership contract. **No stable timing
gain is claimed.**

The follow-up used the same binaries, CPU 0 and warmup. Run each binary as
`flow-before continuation 10000000` / `flow-after continuation 10000000` under
`taskset -c 0`, alternating before/after versus after/before order for seven
repeats. This changes run duration only; it is not a replacement for the initial
unfavorable observations.

## 2. Child blueprint ownership

The async aggregator's reusable factory still owns its blueprint handles.
Each per-submission pack now transfers its handle into the owning child runner,
removing one retain/release pair per launched child. Pending fast `when_any`
losers continue owning their blueprints after the parent/winner is gone.

The four-synchronous-child fast `when_all` probe measured:

| Before ns/aggregation | After ns/aggregation | Median time change |
| ---: | ---: | ---: |
| 238.143 [180.818–248.404] | 190.722 [168.705–248.396] | −19.9% |

This is launch/ownership bookkeeping, not concurrent fan-out throughput. The
ranges overlap. The mechanical retain/release reduction and dedicated lifetime
tests are stronger evidence than treating the median as a universal 20% gain.

## 3. Identity routing: topology determines the answer

Both variants are the **new implementation**, with one or four outgoing routes.
This comparison does not separately prove zero default-path overhead relative
to the original single-route implementation. One releaser returns blocks to
live but parked allocating owners. Only deallocation and explicit batch flush
are timed; this is not a concurrent allocator-throughput measurement.

Units below are ns/released block, summarized from each process's median
round-average cost.

| Origins / order | Routes 1 median [range] | Routes 4 median [range] | Median time change |
| --- | ---: | ---: | ---: |
| 1 / grouped | 6.031 [5.875–6.188] | 6.258 [4.773–6.535] | +3.8% |
| 1 / interleaved | 9.008 [8.844–9.312] | 8.613 [6.492–8.844] | −4.4% |
| 2 / grouped | 6.045 [5.949–6.145] | 6.418 [6.301–6.617] | +6.2% |
| 2 / interleaved | 14.949 [11.777–15.459] | 8.063 [7.672–8.412] | **−46.1%** |
| 4 / grouped | 6.163 [5.998–6.223] | 6.331 [6.193–6.604] | +2.7% |
| 4 / interleaved | 15.281 [14.607–15.751] | 8.600 [8.375–8.679] | **−43.7%** |
| 8 / grouped | 6.056 [5.822–6.350] | 6.433 [6.238–6.628] | +6.2% |
| 8 / interleaved | 14.598 [14.006–15.194] | 17.078 [16.887–19.010] | **+17.0%** |

Two/four interleaved origins improve in all five matched repeats, with disjoint
ranges. Eight interleaved origins regress in all five, also with disjoint ranges.
The separate histogram explains this: routes4 restores batch16 for two/four
origins, but eight round-robin origins evict lanes and remain at batch1. For four
origins, 2,048 returned blocks require 2,048 versus 128 publications. That is a
16-fold reduction in publications, not a 16-fold throughput gain. All histogram
returns went to live origins; no adoption performance is inferred.

One route remains default. Four routes increase pending retention from at most
15 to 60 dead blocks per thread/pool specialization at batch16, plus metadata.
They are appropriate when a small repeated working set of allocation origins
is interleaved and that retention is acceptable. Pending batches hold no
publisher tickets. All-lane flush, generation validation, full-inbox fallback,
stale-origin adoption and close quiescence retain their existing protocols.

The raw `p99_ns_per_release` is a percentile of **round averages**, not of
individual release latencies. Outliers remain large and do not establish a tail
latency benefit. One-origin order variants also have different traversal code;
their absolute difference is not an origin-batching effect.

## 4. Simple executor: a clear drain result, mixed scheduled tails

The successful-pop loop now resets backoff and immediately tries the next task;
idle backoff advances only after a failed pop. Queue ordering, pending-work
accounting and shutdown checks are unchanged.

| Prefilled 1,024-task drain | Median ns/task | Complete process range |
| --- | ---: | ---: |
| Before | 55.772 | 43.904–61.566 |
| After | 15.763 | 13.094–28.423 |

All five pairs improve. Median measured drain cost is 71.7% lower. This timed
region excludes filling the queue, concurrent enqueue traffic and idle periods;
it cannot be advertised as an application-wide 3.54× throughput gain.

The scheduled cross-thread matrix used 32,768 samples, capacity 1,024, work 32,
three runner modes and offered rates 100k/500k/2M. At 100k/500k, completion rate is
mostly arrival-capped. At 2M, normal-runner completion medians improve from
1.294M/s to 1.464M/s, but ranges are broad: 0.894–2.000M/s and 1.378–2.000M/s.

All nine planned-arrival p99 ranges overlap. Eight medians improve, but raw 2M's
median worsens from 1.586 to 3.104 ms. Fast 500k's actual-submit p99 median worsens
from 88.2 to 130.6 µs. One fast 500k candidate process has a 26.329 ms planned-arrival
p99 alongside 26.309 ms submit-lag p99; that outlier remains in the data and cannot
be attributed solely to executor service. The before/after scheduled binaries
also include the broader library changes, rather than isolating only the pause.

Keep the progress-path correction. Report the isolated drain improvement and
the mixed scheduled observations separately; no general tail improvement is
established.

## 5. UV: explicit batching helps; fence timing is unresolved

Both modes construct identical task groups and use explicit yield-on-full
retries. Batch 16, 131,072 tasks per producer, capacity 65,536 and budget 4,096:

| Producers / policy | Single ns/task, median [range] | Batch 16 ns/task, median [range] | Median time change |
| --- | ---: | ---: | ---: |
| 1 / automatic | 165.318 [121.630–183.046] | 80.553 [55.212–136.850] | −51.3% |
| 1 / always fence | 148.332 [81.253–213.118] | 61.038 [49.493–95.150] | −58.9% |
| 4 / automatic | 192.217 [142.830–238.745] | 122.593 [97.695–567.087] | −36.2% |
| 4 / always fence | 221.546 [168.755–251.612] | 160.617 [118.188–206.154] | −27.5% |

The forced-fence controls improve too, so batching's signal does not depend on
fence removal. The 4-producer automatic case wins only 3/5 pairs and includes a
567.087 ns/task outlier. Width 1 is inconclusive: 97.398 versus 90.867 ns/task with
broad overlap. Retry counters have different units in single and batch modes,
so their ratio is not a like-for-like contention metric.

Keep `try_dispatch_batch()` explicit for tasks already available as a group.
It enqueues on the owner thread too, returns the accepted prefix, leaves the
suffix intact, and holds one admission ticket through the final native send.
Concurrent producers can interleave tasks. The default drain budget remains 16;
the measured budget 4,096 does not validate unrelated-I/O fairness or justify
waiting for additional tasks to manufacture batches.

Automatic versus forced fences is mixed: automatic single submission costs
11.5% more at 1 producer and 13.2% less at 4 producers in the primary medians.
No independent speed benefit is established. The policy is retained because
[libuv documents sequentially consistent send/callback ordering, including
coalescing, from 1.53.0](https://docs.libuv.org/en/v1.x/async.html).
The linked runtime version is checked once at construction; older runtimes keep
the FF fence. `always_fence` remains available. Queue/admission atomic orders
were not weakened.

## 6. Controller storage: a conditional allocation win

All variants execute a fresh normal runner around the same synchronous flow.
The eager system and pooled variants both use the new allocator-tag factory,
separating allocator choice from the old factory's lazy initialization placement.

| Policy | Median ns/op | Complete process range |
| --- | ---: | ---: |
| Existing lazy system | 40.688 | 29.135–48.796 |
| Eager system allocator | 39.394 | 29.335–64.092 |
| Eager pooled allocator | 24.410 | 21.792–49.177 |

Pooled wins all five eager-system matched repeats. The ratio of medians shows
38.0% lower cost; the median of the five paired reductions is 25.7%, a different
statistic. A separate instrumented run observes 100,000 `posix_memalign` calls
for each system policy and 0 for warmed pooled storage across 100,000 operations.
Those are aligned backing-allocation counts, not proof of zero construction or
zero allocation work.

This favorable case has one live controller at a time and same-thread final
release. The default allocator remains unchanged. Pooling is selected with
`make_runner(std::allocator_arg, pooled_controller_allocator<>{}, ...)`.
Only the last controller owner recycles storage; retained cancellation handles
remain valid, and late cancellation cannot target a new operation reusing a
completed receiver's storage. Remote/retired-origin returns and fresh cancel
state are covered by contracts and ThreadSanitizer, without implying the same
performance under delayed release or a large live set.

## 7. Terminal-only I/O: explicit semantic choice

Both sides below use fast runners; blueprint ownership is held constant. These
are **staged userspace** io_uring submission/routing measurements, with synthetic
completion delivery and no kernel I/O:

| Batch / blueprint | Ordinary ns/op, median [range] | Terminal ns/op, median [range] | Median time change |
| --- | ---: | ---: | ---: |
| 1 / owning | 70.495 [54.908–105.476] | 70.273 [55.877–94.649] | −0.3% |
| 1 / borrowed | 72.778 [45.973–89.313] | 69.149 [45.443–98.336] | −5.0% |
| 32 / owning | 69.712 [49.425–79.017] | 59.595 [41.997–70.146] | −14.5% |
| 32 / borrowed | 65.067 [54.100–69.171] | 44.903 [39.430–100.158] | −31.0% |

Batch 1 has no resolved improvement. Batch 32 is a positive but noisy signal,
winning 3/5 pairs for each ownership mode. The larger normal-runner/fast-runner
difference is not a new terminal-adapter gain. Borrowing a blueprint is a
separate lifetime choice, not a free optimization to combine into that claim.

The public `await_io_uring_fast` and `await_dpdk_rx_fast` adapters preserve one
terminal completion per accepted request and no later callback after rejection.
They omit the ordinary adapter's backend retain/release pair and early logical
cancellation competition. Controller cancellation does not end accepted fast
I/O early; buffers/packets remain owned through terminal backend completion.
DPDK context close still produces terminal cancellation on the next poll.
Ordinary factories remain unchanged.

### Native backend limits

- **io_uring:** initialization returned `-EPERM`; the driver returned 77 and both
  integrated native-runtime cases skipped. No kernel throughput was measured.
- **DPDK:** headers, fake-API contracts and benchmark compile passed, but EAL
  initialization failed because no lcores were detected; the benchmark exited 77.
  This environment has no `/sys`; DPDK 25.11
  checks `cpuN/topology/core_id` for core detection. Ordinary core-list/remap
  flags use the same check, so no valid native `net_ring` run was possible.
  There is no DPDK performance result here.

## Correctness and validation

The selected integrated suite reports **38 passed, 2 skipped**. Skipped kernel
tests are not counted as passing. Added normal/strict-no-exception contracts,
route4 integration targets, and standalone public-header builds pass GCC C++14
with warnings as errors. The six new controller cases include explicit
allocator failure. Existing pipeline/reuse/cancellation tests were included in
the integrated run.

Focused address/undefined-behavior sanitizer runs passed for the major changed
lifecycle paths. LeakSanitizer aborts under the environment's ptrace setup and
was disabled; no leak-sanitizer coverage is claimed. Pool/backend ownership
counts are still asserted by the tests. Identity multi-origin/exit races and
all six controller cases separately passed ThreadSanitizer.

A GCC 14 optimized warning in the DPDK test's inactive union storage was resolved
by a test-only structure change: owning deferred-null error delivery, borrowed
deferred success, and separate borrowed null rejection/reuse remain covered.
No production packet zeroing, generic union change or warning suppression was
introduced to obtain the clean build.

This local run does not establish MSVC, ARM weak-memory, physical-NIC, complete
production cancellation-load or sustained service-latency behavior. The existing
CI/sanitizer workflow remains responsible for its wider matrix; the nightly
TSan target list now includes the added ownership/controller/route4 regressions.

## Reproduction

Follow the [driver documentation](../../README.md) and install the optional
backend development packages. The primary matrix was:

```sh
python3 bench/optimization_pass/run.py \
  --out /tmp/ff-optimization-pass \
  --baseline a93d246981a49648949c489a1918f5ccb61ed15d \
  --repeats 5 --cpu 0 --build-only

python3 bench/optimization_pass/run.py \
  --out /tmp/ff-optimization-pass \
  --baseline a93d246981a49648949c489a1918f5ccb61ed15d \
  --repeats 5 --cpu 0 --run-only
```

Use permitted CPUs and keep all other compilation/benchmark jobs stopped during
timing. The driver stores exact commands, complete stdout/stderr, runtime skips,
raw rows and summaries. Instrumentation is a separate binary; empty controller
CSV fields mean unmeasured, not zero. Reproduce on the intended workload/hardware
before enabling workload-dependent policies globally.
