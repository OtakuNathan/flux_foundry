# Cross-core UV dispatch and scheduled flow handoff

Retained optimization: `uv_executor::try_dispatch` borrows `state_.get()`
instead of copying the internal `shared_ptr` on every attempt. The public
object already has to outlive concurrent member calls. For owner-inline
callbacks that destroy that object, the native handle's independent owner
retains the state until its close callback. Recursive `uv_run` is prohibited.
The existing admission ticket still spans native notification. Queue atomics,
memory ordering, cancellation behavior and the close algorithm are unchanged.

The [patch](uv-state.patch) removes two shared-reference-count atomic updates
per dispatch attempt. It adds no new API or user obligation. Existing task
move/destruction and always-lock-free counter `static_assert` checks remain.

## Saturated dispatch throughput

3700X, owner CPU 3, capacity 1024, callback budgets 16 and 64. CPUs 0–3 share
one L3 cache; CPUs 4–7 share another. These runs use distinct physical cores,
not SMT siblings. The owner-inline case has no queue handoff and is a separate
single-thread reference, not a same-core two-thread queue benchmark.

Seven alternating before/after trials per case, 1,048,576 tiny tasks per trial,
after 16,384 warmup tasks. Both compilers use C++14 `-O3 -DNDEBUG`; task counts,
per-producer order, retained rejection and clean close are checked. Values
below are medians in ns/task; change is **time reduction**, not throughput gain.

| Compiler | Placement | Budget | Before → after | Time reduction |
|---|---|---:|---:|---:|
| Clang 23 | Owner inline | 16 | 18.0 → 10.2 | 43.7% |
| GCC 16 | Owner inline | 16 | 17.9 → 9.4 | 47.6% |
| Clang 23 | One producer, same L3 group | 16 | 121.0 → 106.8 | 11.8% |
| GCC 16 | One producer, same L3 group | 16 | 116.0 → 104.4 | 10.0% |
| Clang 23 | One producer, other L3 group | 16 | 105.8 → 101.8 | 3.8% |
| GCC 16 | One producer, other L3 group | 16 | 140.3 → 116.2 | 17.2% |
| Clang 23 | Three producers, same L3 group | 64 | 151.2 → 85.6 | 43.3% |
| GCC 16 | Three producers, same L3 group | 64 | 159.1 → 67.8 | 57.4% |
| Clang 23 | Three producers, other L3 group | 64 | 182.3 → 136.2 | 25.3% |
| GCC 16 | Three producers, other L3 group | 64 | 182.5 → 77.1 | 57.8% |

All 20 scenario/compiler median comparisons improve. Cross-core magnitudes
depend strongly on compiler, placement and callback budget; they are not a
general application speedup. Retry counts and full timing ranges are retained.

[All 280 rows](throughput/results.csv), [full medians and ranges](throughput/summary.md),
[compile commands and compared header hashes](throughput/environment.json),
[host topology and source hashes](host.json).

## Atomic instruction confirmation

Three separate owner-inline perf-stat runs per version/compiler, 16,777,216
measured tasks plus 16,384 warmup tasks. The same four Zen 2 retired-lock
events used in the earlier profiles were grouped, user-space only. Every
event ran 100%; bus-lock counts were zero. Counts include process setup and
closure, so normalized absolute totals contain a small fixed overhead.

Both compilers give median totals of 4.000012 locked instructions/task before
and 2.000012 after, normalized by measured plus warmup tasks. The difference
is exactly two per task. [Raw counter outputs](counters/) retain all 12 runs.

Command shape, with the extracted perf library environment:

```sh
perf-root/usr/bin/perf stat -x ';' \
  -e '{ls_locks.non_spec_lock:u,ls_locks.spec_lock_hi_spec:u,ls_locks.spec_lock_lo_spec:u,ls_locks.bus_lock:u}' \
  -- build/cross-core-final/clang++-after after 0 16777216 1024 16 3 0
```

## Scheduled-arrival latency

The new [production-UV handoff probe](../../../uv_handoff.cpp) reuses the
existing scheduled-arrival measurement helpers, including checksum and
exactly-once checks and a completion fence before destroying sample storage.
It compares raw tasks with **ordinary awaitable + owning fast runner**; the
awaitable type is unchanged. It explicitly retries retained full-queue tasks
for a finite no-drop schedule. Production `dispatch` still rejects overflow.

Five alternating trials, 32,768 events each, after 4,096 warmup events. Matrix:
both compilers; producer CPU 2 or 4, owner CPU 3; budgets 16/64; work 0/128;
planned arrival rates 500,000/s and 2,000,000/s. The work is the existing
unsigned dependency-chain synthetic payload. Producer scheduling busy-waits.
Timestamping, completion accounting and the rest of the harness add overhead
to these measurements; they are not uninstrumented service latency.

At 500,000/s with work 128 and ordinary awaitable + fast runner, median
planned-arrival p99 stays around 3.7 microseconds within one L3 group and
5.2–5.4 microseconds across groups. The dispatch optimization is useful for
saturated throughput but does not deliver a comparable reduction in p99 here.
P999 moves in both directions; no universal tail-latency improvement is claimed.

At 2,000,000/s, cross-group ordinary-awaitable handoff cannot keep up with the
schedule. For the retained version, budget 16, work 128:

| Compiler | Producer CPU | Completed/s | Planned-arrival p99 | Actual-submit p99 | Submission-lag p99 |
|---|---:|---:|---:|---:|---:|
| Clang | 2 | 2.00 M | 2.84 us | 2.17 us | 0.35 us |
| Clang | 4 | 1.05 M | 14.54 ms | 4.92 us | 14.54 ms |
| GCC | 2 | 2.00 M | 2.56 us | 1.91 us | 0.57 us |
| GCC | 4 | 1.22 M | 10.28 ms | 5.87 us | 10.27 ms |

These quantiles are computed independently and should not be added or
subtracted to reconstruct individual events. The large planned-arrival tail
is principally accumulated producer lateness in this finite schedule, not
ten milliseconds spent servicing an already-submitted event. Raw tasks keep
up at about 2 M/s in these same cases, though their tail latency also varies.
This is a concrete reason to investigate cross-core flow construction and
ownership handoff; it does not isolate a particular atomic or cache line.

[All 640 latency rows](latency/results.csv), [median comparison](latency/summary.md),
[full per-metric min/median/max](latency/summary.csv),
[compile commands](latency/environment.json).

## Allocation check

Separate instrumented runs count executable-origin malloc, aligned-allocation
and global-new calls **on the producer thread only**, inside the submission
loop. Thread-local counters avoid racing the consumer. Shared-library internals
are not intercepted; timings from these instrumented runs are discarded.

At 500,000/s, work 128, producer CPU 4 / owner CPU 3, both 4,096- and 32,768-event
runs show zero tracked system-allocation calls for raw and ordinary-fast lanes.
The normal-runner lane performs one aligned allocation per event for its
controller. Thus per-request system allocation is not established as the cause
of the ordinary-fast cross-group limit. Pool internals can still have costs.

[Allocation counts](allocations/alloc.log), [link command](allocations/environment.json),
[thread-local wrapper source](allocations/producer_alloc.cpp), and
[instrumentation-window patch](allocations/window.patch) are retained. The
wrapper was derived from the earlier single-thread diagnostic; its legacy
comment describes that origin, while the actual counters here are thread-local.

## Correctness and reproduction

The UV contract adds direct and nested owner-inline public-object destruction,
so one or two dispatch frames still hold admission tickets during destruction.
Existing coverage includes paused native sends, unpublished reservations,
close races, queued destruction, rejection ownership, non-power-of-two queues,
allocation failure and normal/fast runners.

Desktop Clang C++14 normal, strict no-exceptions/no-RTTI, ASan+UBSan and TSan
builds all pass. TSan skips the test's global allocation replacement as before;
that path is covered by the other builds. [Logs](tests/) are retained. The
same contract also passes GCC C++14 on the Pi ARM host with system libuv 1.50.0.

Desktop test flags: `-std=c++14 -O1 -g -pthread -Wall -Wextra -Werror`, after
overlay first, root include second, and `-luv`. Strict mode adds
`-fno-exceptions -fno-rtti -DFLUX_FOUNDRY_NO_EXCEPTION_STRICT=1`; ASan+UBSan adds
`-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer`;
TSan adds `-fsanitize=thread -fno-omit-frame-pointer`. The Pi uses the normal
flags with `c++`, `-I.` and the current header. No close-state transition or
memory order changes, so the existing TLA+ protocol model is unchanged; it
does not by itself model the shared-pointer lifetime argument above.

The exact host-specific drivers are [screen_uv.py](screen_uv.py) and
[compare_handoff.py](compare_handoff.py). From the preserved desktop scratch
checkout `/tmp/ff-io-perf-vFZ6HVwW`, the final throughput command is
`python3 screen_uv.py --out build/cross-core-final --repeats 7`;
latency uses `python3 compare_handoff.py`. They create fresh output directories,
use saved before/after UV headers, and use the prior optimized runner overlay
for latency. Their CPU placements deliberately match the recorded topology.

Multi-producer measurements here are throughput tests; latency distributions
use one producer. There is no physical NIC traffic, multi-loop sharding,
timer/I/O fairness measurement, CPU isolation or real-time scheduling. Larger
callback budgets can affect fairness, which this probe does not establish.
