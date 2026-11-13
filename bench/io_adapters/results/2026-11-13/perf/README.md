# 3700X perf: the remaining cost is measurable

Hardware counters and sampling confirm substantial per-operation bookkeeping
in these tiny workloads. Pooling is working: the earlier allocation trace
showed no per-request system allocations in the warmed fast-runner lanes.
The most directly established cost here is atomic ownership/state handling.

## Scope and method

Same 3700X, Clang 23 io_uring binaries, GCC 16.2.1 DPDK binaries and libraries
as the diagnosis. perf 7.3.1 was extracted into a temporary directory. No
privilege, sysctl, governor or device configuration changes were needed.
Sampling uses `--user-callchains` because default callchain settings attempted
kernel profiling, which this host's `perf_event_paranoid=2` disallows.

Three rotated trials, 16,777,216 operations/bursts per lane, pinned to CPU 3.
Control/ack pipes enable counters after warmup and disable them at completion;
setup, EAL and teardown are excluded. Small control-ack overhead remains at
the interval boundaries. All event running percentages are 100%; no event
multiplexing was observed. All counters are **user-space only**. Real-NOP
lanes therefore exclude kernel io_uring processing and cannot be interpreted
as whole-request CPU cost.

Ten selected lanes also record `cycles:u` at 997 Hz with DWARF user callchains.
Their reports show zero lost samples. Sampling may skid: a hot instruction
immediately after a locked operation is not proof that the following
instruction itself consumed all those cycles. Source lines and neighboring
instructions are inspected together.

## Atomic instructions: directly measured

Zen 2 exposes four retired-lock categories. Summing them gives the following
per-operation medians; bus-lock counts were zero throughout. These are cached
atomic RMW instructions, not evidence of mutex contention or bus locking.

| Lane | Retired locked instructions/unit | User cycles/unit in this counter run |
|---|---:|---:|
| io_uring staged, ordinary awaitable + fast runner | 15.00000 | 335.07 |
| io_uring staged, fast awaitable + fast runner | 11.00000 | 262.18 |
| io_uring staged, fast awaitable + borrowed bp | 5.00000 | 152.13 |
| DPDK single-packet burst, ordinary awaitable + fast runner | 13.00000 | 286.47 |
| DPDK single-packet burst, fast awaitable + borrowed bp | 3.00000 | 146.62 |

Source inspection explains the count differences: the terminal-only node
removes two ordinary-awaitable state CAS operations and an extra backend
retain/release pair. Borrowed blueprint storage removes six ownership RMWs
through the factory, runner and continuation. The terminal io_uring path still
has two context-ticket RMWs and three fast-awaitable reference operations;
the DPDK path has those three awaitable reference operations without an
io_uring admission gate. These totals agree with hardware counts.

The generic counter pass also shows the effect is larger than instruction
count reduction alone:

| io_uring staged lane | cycles/request | instructions/request | IPC |
|---|---:|---:|---:|
| ordinary awaitable + fast runner | 335.20 | 366.69 | 1.09 |
| fast awaitable + fast runner | 261.89 | 340.69 | 1.30 |
| fast awaitable + borrowed bp | 151.86 | 307.53 | 2.03 |

For the last row versus the first, retired instructions fall about 16%, while
cycles fall about 55%. Atomic serialization is supported by the ablation,
retired-lock counts and annotated hotspots together, not just by one sample.
Branch misses are about 0.063/request in all three staged lanes and do not
explain this gap. DPDK single-packet branch misses are below 0.0001/burst.

## Sampled hotspots

In the original io_uring **staged** lane, self cycle-sample shares include:

| Symbol / region | Self sample share |
|---|---:|
| `measure<3>` including inlined bp ownership and CQ routing | 28.95% |
| ordinary `awaitable_base::access_delegate::submit_async` | 24.04% |
| public `io_uring_awaitable::on_complete` | 12.23% |
| `io_uring_context::enqueue` | 7.50% |
| awaitable factory | 7.46% |
| continuation invoker | 7.34% |
| `awaitable_base::do_resume` | 5.87% |
| cached allocator `alloc()` itself | 0.52% |

Annotation of submit/completion places most samples at or immediately after
the state CAS and retain/release instructions. Annotation of the outer loop
likewise places many samples around blueprint `lock inc/dec` operations.
The DPDK single-packet ordinary lane shows the same pattern in its more heavily
inlined `measure<1,4>` function.

The low allocator-function self share does **not** mean total object lifecycle
cost is zero. Constructors, continuations and destructors reside elsewhere.
In the terminal+view io_uring lane, a request-construction load/store sequence
is conspicuous in the factory annotation. The Zen 2 store-to-load-interlock
counter remains around 2.22 events/request (ordinary lane around 2.74).
These are conflict-event counts, not stall-cycle counts; they do not by
themselves establish which particular copy causes each interlock.

## DPDK burst-32: do not charge the payload to the runner

The fast-node + borrowed-bp report assigns 44.53% self samples to a
`flow_fast_runner::ipc::run` instantiation. Source annotation shows much of
that region is the benchmark receiver's **packet length check, packet-data
load and checksum/count update**, inlined into the continuation. This is not
44.53% pure runner scheduling overhead. The raw lane also spends 49.02% in
its corresponding measured function, with the same packet-checksum loop hot.

Another 38.80% of terminal+view samples (34.98% in raw) land in the software
ring PMD at offset `0x2fcb`. Disassembly places that sample in the loop updating
each received mbuf's port metadata. PMD/library internal symbols are stripped,
so the address and disassembly are retained instead of inventing a symbol name.

Burst-32 generic counters are 1176 cycles/burst raw, 1421 ordinary+fast runner,
and 1292 terminal+view. Their generated layout and sampling/instrumentation
differ from the initial timing snapshot; these are a separate comparison.
The owning terminal variant was slower than ordinary in this particular
burst-32 counter pass. Explicit fast contracts do not guarantee that every
compiled workload gets faster. Keep the layout-sensitive burst results and
the clearer single-packet ownership findings separate.

## Actionable follow-up

The first low-risk candidate is the existing `make_fast_runner(lite_ptr<bp_t>,
receiver)` factory: its parameter already owns a passed-in reference, but
constructs `bp_storage(bp)` by copying it again. The receiver-less owning
overload has the same pattern. Moving that by-value parameter into storage
would target one redundant retain/release pair without borrowing the blueprint
or weakening its lifetime contract. This report does not implement or claim
validation of that change.

Further reductions to awaitable references require preserving completion
during submit, cancellation and backend lifetime contracts. The experimental
terminal-only node and borrowed blueprint deliberately have different contracts;
they are not automatic replacements. Pool removal is not indicated by this
evidence.

Follow-up: the redundant factory copy was subsequently removed and measured;
see the [owning-runner move optimization](../runner-move/README.md).

## Evidence and reproduction

- [All generic counter medians](counters.md), [normalized trials](normalized.csv),
  [environment / exact benchmark lanes](environment.json), raw events in `stats/`.
- [Atomic/interlock medians](atomics/counters.md),
  [CPU event definitions](atomics/event-descriptions.txt), raw trials in `atomics/`.
- [Original io_uring self samples](reports/uring-staged-current-self.txt),
  [fast+view self samples](reports/uring-staged-terminal_view-self.txt).
- [Submit annotation](annotations/uring-staged-current-annotate-1.txt),
  [completion annotation](annotations/uring-staged-current-annotate-2.txt),
  [bp-loop annotation](annotations/uring-staged-current-annotate-0.txt),
  [fast-node factory annotation](annotations/uring-staged-terminal_view-factory-annotate.txt).
- [DPDK single-packet annotation](annotations/dpdk-1-current-annotate-0.txt),
  [raw burst-32 payload](annotations/dpdk-32-raw-annotate-0.txt),
  [fast+view burst-32 payload](annotations/dpdk-32-terminal_view-annotate-0.txt),
  [PMD metadata-loop disassembly](net-ring-loop.asm).

See the [harness instructions](../../../README.md). Self report whitespace is
compacted for storage; inclusive reports are gzip-compressed. Full unmodified
reports and perf `.data` recordings remain on the desktop at
`/tmp/ff-io-perf-vFZ6HVwW/build/perf-adapters/`. The recordings are excluded from
the repository. Public adapters, runner and original benchmark source were
not changed for this investigation.
