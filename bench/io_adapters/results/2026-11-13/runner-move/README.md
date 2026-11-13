# Move the already-owned blueprint into fast-runner storage

Both `make_fast_runner(lite_ptr<bp_t>, ...)` overloads now construct storage
with `bp_storage(std::move(bp))`. The by-value parameter already owns a
reference, so copying it again was redundant. Lvalue callers retain their own
pointer; rvalue callers transfer theirs. The runner continues to own the
blueprint through asynchronous completion. Awaitable types, cancellation
behavior and reference-count memory ordering are unchanged.

This implements the small factory optimization identified by
[perf](../perf/README.md). The experimental terminal-only nodes remain
benchmark types, and borrowed blueprint storage is not introduced by this fix.

## Validation and measurement

Five rebuilt GCC 14 C++14 Debug contracts passed on the Pi:
`runner_reuse_contract`, `awaitable_lifecycle_contract`,
`awaitable_lifecycle_contract_noexc`, `pipeline_lifetime_enabled`, and
`pipeline_lifetime_noexc`. They cover runner handoff/reuse and asynchronous
lifetime behavior, including strict no-exception builds. A build interrupted
by a host reboot was resumed before testing.

On the 3700X, both versions were rebuilt from identical probe sources and
compile recipes. An isolated header overlay changes only `flow/flow_runner.h`;
the baseline checkout remains intact. Seven warmed trials of 1,048,576
operations alternate before/after order and node order. io_uring uses Clang 23,
DPDK GCC 16.2.1, with the prior dependency versions and CPU 3 affinity.

| Workload | Before | After | Median change |
|---|---:|---:|---:|
| io_uring real NOP, ordinary awaitable + fast runner | 154.691 ns/request | 146.165 ns/request | −5.51% |
| io_uring real NOP, experimental fast awaitable + fast runner | 137.506 ns/request | 129.333 ns/request | −5.94% |
| DPDK one-packet burst, ordinary awaitable + fast runner | 68.535 ns/burst | 58.652 ns/burst | −14.42% |
| DPDK one-packet burst, experimental fast awaitable + fast runner | 48.683 ns/burst | 40.443 ns/burst | −16.93% |
| DPDK 32-packet burst, ordinary awaitable + fast runner | 336.419 ns/burst | 309.856 ns/burst | −7.90% |
| DPDK 32-packet burst, experimental fast awaitable + fast runner | 315.904 ns/burst | 309.353 ns/burst | −2.07% |

Real io_uring lanes batch 32 NOPs. DPDK remains native software ring PMD
loopback with packet checks, not physical NIC I/O. Full ranges overlap,
especially at DPDK batch 32; small median changes there are not a universal
throughput guarantee. Natural code layout is retained. The consistent effect
in lighter lanes is roughly 8–10 ns per invocation.

Separate gated counter runs confirm two fewer retired locked instructions per
invocation, with one before/after counter run per case and 100% event running:

| Node + fast runner | Before | After |
|---|---:|---:|
| io_uring ordinary | 15 | 13 |
| io_uring experimental fast | 11 | 9 |
| DPDK ordinary, batch 1 | 13 | 11 |
| DPDK experimental fast, batch 1 | 9 | 7 |

The fixed overhead remains visible for a nearly empty NOP or one-packet flow.
Burst-level DPDK flows amortize it better. These probes establish neither
production tail latency nor physical-NIC/cross-core packet throughput. The
warmed fast-runner path does not perform a system allocation per request.

## Evidence and reproduction

- [Raw timings](results.csv), [medians and full ranges](summary.md),
  [compile commands and header SHA-256](environment.json).
- Counters: [io_uring before](uring-current-before.stat) /
  [after](uring-current-after.stat), [DPDK before](dpdk-current-before.stat) /
  [after](dpdk-current-after.stat). Terminal-node variants are alongside these.
- Run [`compare_runner_move.py`](../../../compare_runner_move.py) from a
  pre-change checkout with `--replacement /path/to/after-flow_runner.h`,
  `--uring build/diag-uring --dpdk build/diag-dpdk --out build/runner-move`,
  and `--perf /path/to/perf`. The prior diagnostic directories supply the
  source and compile recipes; preserve their library environment.

The desktop comparison is retained at
`/tmp/ff-io-perf-vFZ6HVwW/build/runner-move/`, including its saved baseline header.
