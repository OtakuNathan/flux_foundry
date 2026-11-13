# Shared-pool and cross-core follow-up

The shared allocator pool is a measurable remaining cost, but the tested
batch-transfer variants are **not retained as the default implementation**.
They improve cross-cache-group flow throughput while regressing some of the
existing single-core DPDK cases. `memory/pooling.h` is restored byte-for-byte
to the baseline. Candidate patches and results are retained; new ownership
and reclamation tests remain in the suite.

## Profiling evidence

Same 3700X topology as the [UV cross-core study](../cross-core/README.md):
owner CPU 3, producer CPU 2 within the same L3 group or CPU 4 in the other
group. Clang C++14 `-O3 -g`, production optimized UV executor, ordinary
awaitable + owning fast runner, 128 synthetic work iterations and planned
arrivals at 2 M/s. Each recording contains 2,097,152 events after warmup.

User-cycle sampling at 997 Hz uses DWARF user call chains and perf control/ack
pipes. Recording starts at the measured submission loop and ends after its
completion fence, excluding sample allocation, sorting and reporting. All
three recordings report zero lost samples. Some zero-address samples remain
unresolved and are not attributed to a subsystem.

Self cycle-sample shares for the ordinary-fast flow:

| Symbol | Same L3 group | Other L3 group |
|---|---:|---:|
| Shared pool allocate | 4.27% | 8.86% |
| Shared pool deallocate | 2.48% | 4.31% |
| UV try_dispatch | 5.37% | 9.35% |
| Continuation invoker | 5.35% | 9.53% |

The pool accounts for about 13.2% of sampled cycles across groups even though
the earlier allocation trace found no per-event system allocation in this
lane. These are fractions of separate runs with different throughput and
producer pacing; they are not a causal decomposition of the slowdown.
The continuation and backend callback also contain useful work, completion
accounting and lifetime operations, so their total samples are not all FF
scheduling overhead.

Compressed flat reports: [same-group flow](ordinary-fast-cpu2.flat.report.gz),
[cross-group flow](ordinary-fast-cpu4.flat.report.gz),
[cross-group raw task](raw-cpu4.flat.report.gz).
[Compile/record commands](environment.json), [sampling hooks](sampling-window.patch)
and the corresponding run logs are alongside them. Raw recordings remain on
the desktop under `/tmp/ff-io-perf-vFZ6HVwW/build/producer-perf/`.

## Shared-pool batch candidates

The prototype refills an empty TLS cache with a small batch from the existing
bounded shared pool. It never prefetches heap allocations. On a full TLS cache,
it returns a small batch of cached blocks, then caches the newly freed block.
The existing free-list operations, atomics and memory orders are unchanged;
this groups transfers rather than replacing them with a new bulk atomic API.
The transfer count is capped by cache capacity. Over-aligned/large allocations
use the existing separate allocator specialization.

Each final comparison has seven alternating before/after trials of 32,768
events after 4,096 warmup events: GCC and Clang, CPUs 2/4 to owner 3, work 128,
UV capacity 1024/budget 16, planned arrivals 500k/s and 2M/s. Both sides include
the retained runner, UV and packet-prefix optimizations where applicable.

The four-block version gives these cross-group results at 2M/s:

| Compiler | Completed/s before → after | Throughput gain | Planned-arrival p99 before → after |
|---|---:|---:|---:|
| Clang | 1.125 M → 1.456 M | 29.4% | 12.63 ms → 6.10 ms |
| GCC | 1.217 M → 1.514 M | 24.4% | 10.46 ms → 5.25 ms |

It still cannot sustain the requested 2M/s. These millisecond tails largely
reflect accumulated producer lateness in the finite no-drop schedule, as in
the preceding study. At 500k/s, median p99 is broadly similar: Clang same-group
3.58 → 3.61 us, GCC same-group 3.65 → 3.58 us. P999 varies in both directions;
Clang same-group is 5.21 → 5.65 us in this comparison.

[Four-block patch](pool-batch-4/candidate.patch), [112 timing rows](pool-batch-4/results.csv),
[summary](pool-batch-4/summary.md), [full metric ranges](pool-batch-4/summary.csv),
[environment/header hashes](pool-batch-4/environment.json),
[host-specific driver](pool-batch-4/compare.py).

A sixteen-block version also helps cross-group throughput but increases the
Clang 500k/s same-group p99 from 3.56 to 3.87 us.
[Patch](pool-batch-16/candidate.patch), [results](pool-batch-16/results.csv),
[summary](pool-batch-16/summary.md), [ranges](pool-batch-16/summary.csv).
Its data is a separate paired experiment; do not compare absolute throughput
across the two candidate experiments as if they shared the same trials.

## Why it is not the default

Seven warmed single-core controls use the original ordinary-awaitable + fast
runner diagnostic workloads, 1,048,576 operations/bursts, both compilers and
alternating order. io_uring real NOP medians stay within about 0.4%, but the
four-block version increases DPDK single-packet time by 2.4% in GCC and 4.3%
in Clang. Full 32-packet burst medians increase 2.7% and 8.4%, respectively;
the full-burst ranges overlap and are more variable.

[112 control rows](single-core/results.csv), [ranges and medians](single-core/summary.md),
[compile commands](single-core/environment.json). The extracted DPDK library
directory must be present in `LD_LIBRARY_PATH` when reproducing these controls.

Clang introduced an out-of-line deallocation call in the initial batch
prototype. Two further three-trial controls checked possible remedies:

- [FORCE_INLINE variant](inline-controls/candidate.patch): no consistent
  recovery; [results](inline-controls/summary.md).
- [Separated non-inlined refill/spill helpers](split-controls/candidate.patch):
  single-packet DPDK becomes essentially neutral (~0.3%), but the GCC
  32-packet median still rises about 6.5%; [results](split-controls/summary.md).

These smaller screens are not proof of a universal regression. They are
enough to withhold a global default change whose net benefit has not been
established across the workloads being optimized. A workload-specific allocator
policy remains a reasonable future direction; no such public option is added
by this experiment.

## Other rejected candidate

Separating UV occupancy, producer tail and consumer head with cache-line-sized
byte gaps preserves the queue protocol and avoids C++14 over-aligned allocation
requirements. It materially regresses multi-producer throughput in this
screen, despite occasional single-producer gains. The patch is unapplied.
[Patch](queue-spacing/candidate.patch), [120 timing rows](queue-spacing/results.csv),
[summary](queue-spacing/summary.md), [commands/hashes](queue-spacing/environment.json).

## Validation and reproduction details

The new `pooling_handoff_contract` and strict-no-exception counterpart exercise
cache capacities 1, 2, 3, 4, 16 and 512, with one allocating thread and one
freeing thread. Payload sequences and all payload words detect premature reuse
or aliasing. Draining/counting the shared pool verifies complete reclamation
after TLS destruction. Separate phases use shared-pool blocks, force malloc
fallback by exhausting that pool, and mix both origins. Compile-time checks
preserve the allocator's `noexcept` interface.

The four-block prototype passes Clang C++14 normal, strict no-exception/no-RTTI,
ASan+UBSan and TSan runs, plus the existing eight-thread allocator stress test.
It also passes four Pi GCC C++14 CTest cases including awaitable lifecycle.
After restoring the production pool, the two new CTest contracts are rebuilt
and passed again. Logs in [tests](tests/) distinguish prototype and baseline.
Passing ownership tests does not override the performance tradeoff above.

Desktop contract flags are `-std=c++14 -O1 -g -pthread -Wall -Wextra -Werror`,
with the prototype overlay first. Strict mode adds `-fno-exceptions -fno-rtti
-DFLUX_FOUNDRY_NO_EXCEPTION_STRICT=1`; ASan+UBSan adds `-fsanitize=address,undefined
-fno-sanitize-recover=all -fno-omit-frame-pointer`; TSan adds
`-fsanitize=thread -fno-omit-frame-pointer`. The existing stress executable runs
as `pooling_allocator_stress 200 8 64 0`.

Allocator comparisons use **real directory copies** for header overlays.
Directory symlinks followed by relative `../` includes can escape an overlay
and include the baseline pool first. An initial affected screen was discarded;
all allocator results retained here use copied directories and checked include
paths. Source hashes identify the candidates; CPU topology is unchanged from
the preceding study. No physical NIC or production traffic is involved.
