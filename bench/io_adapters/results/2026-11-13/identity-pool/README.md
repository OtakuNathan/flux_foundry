# Awaitable identity pool: capacity and latency tradeoffs

Retained: headerless storage with an identity member on the awaitable, a
process-static registration table, TLS local/incoming free lists, batched
remote publication, and adoption after the original thread exits. The policy
is enabled by default with **256 local slots, 256 incoming slots and a batch
of 16**. `FLUX_FOUNDRY_AWAITABLE_IDENTITY_POOL=0` restores the legacy policy.
The generic allocator and BP/executor interfaces are unchanged.

The previous verified I/O and executor work was committed as `1139206` before
this experiment. See the [pool contract](../../../../../docs/identity_pool.md)
for identity lifetime, exact-type allocation, partial-batch flushing and limits.

## Why 256, rather than the highest throughput result

Ryzen 3700X, UV consumer CPU 3, allocating producer CPU 2 (same L3 group) or
CPU 4 (other group). Ordinary awaitable + owning fast runner, 128 synthetic
work iterations, UV queue 1024/budget 16. Seven alternating-order trials per
configuration, 32,768 measured requests after 4,096 warmup requests, Clang 23
and GCC 16. The candidate's local/incoming capacities vary together; sender
batch size remains 16. The legacy allocator's actual TLS capacity is 512.

Across L3 groups, at an offered rate of 2M/s, medians are:

| Policy / capacity | Clang completed M/s | GCC completed M/s | Clang actual-submit p99 µs | GCC actual-submit p99 µs |
|---|---:|---:|---:|---:|
| Legacy / 512 | 1.053 | 1.220 | 4.51 | 6.83 |
| Identity / 128 | 1.410 | 1.430 | 5.71 | 6.89 |
| **Identity / 256** | **1.419** | **1.628** | **5.20** | **4.45** |
| Identity / 512 | 1.371 | 1.915 | 4.94 | 113.69 |
| Identity / 1024 | 1.427 | 1.433 | 4.69 | 5.40 |

512 was initially the throughput candidate, but looking at actual-submission
latency changes the choice. Several GCC 512-slot trials feed the executor
fast enough to build a substantial queue: peak outstanding requests were
159–250 in five of seven trials. With 256, six of seven trials peak at 13–17;
one trial still peaks at 233 with an actual-submit p99 of 112.55 µs. **256 is
a workload-specific compromise, not a guarantee against tails or a universal
optimal capacity.** It uses half the local/incoming capacity of 512 while
preserving a substantial throughput improvement in both compilers here.

The default's completion-rate improvement is about 34.7% in Clang and 33.5%
in GCC. Neither default case sustains the requested 2M/s. Planned-arrival p99
falls from 14.57 to 6.64 ms in Clang and 10.37 to 3.70 ms in GCC. These are
finite, no-drop overload runs: much of that latency is producer lateness,
before actual submission. Actual-submit and planned-arrival latency must not
be substituted for each other.

At 500k/s, all capacities sustain the offered load. Default cross-group p99
is 5.50 µs versus 5.24 µs in Clang, and 5.37 versus 5.38 µs in GCC. Within the
same L3 group, default p99 is 3.68/3.72 µs for Clang/GCC. There is no stable
latency or throughput benefit that justifies increasing the default to 1024.

Variation is material: default cross-group completion rates span 1.264–1.581
M/s in Clang and 1.411–1.997 M/s in GCC. The earlier three-trial screen ranked
some capacities differently. No outliers were discarded. These measurements
do not identify a cache-miss mechanism or prove that L3 capacity caused the
differences.

- [Seven-trial confirmation: all 280 rows](capacity-confirm/results.csv),
  [summary](capacity-confirm/summary.md), [min/median/max](capacity-confirm/ranges.csv),
  [commands, compilers and header hashes](capacity-confirm/environment.json).
- [Three-trial capacity screen](capacity-screen/summary.md),
  [raw rows](capacity-screen/results.csv).
- [Initial five-trial policy-only comparison at capacity 512](initial/summary.md),
  [raw rows](initial/results.csv). This predates the capacity selection and
  is not the default-configuration result.

## Space and layout

For the UV handoff probe's ordinary awaitable, `sizeof` changes from 112 to 128 bytes;
the identity itself is one 8-byte `size_t`, with padding accounting for the
rest. Both still fit FF's 128-byte shared-pool size class. The layout probe's fast
awaitable remains 112 bytes because the identity fits existing layout space.
Other awaitable types can cross a size-class boundary; this is not free
metadata for every type. There is no hidden allocation prefix.

The default TLS pool can retain at most 256 local + 256 incoming + 15 outgoing
dead blocks per specialization, excluding live objects. For 128-byte backing
blocks this is about 65.9 KiB of free blocks plus roughly 2 KiB of local pointer
slots. The legacy 512-slot TLS cache allowed 64 KiB of free blocks plus 4 KiB
of pointer slots. These are bounds, not eager allocations or measured RSS;
the registry, fixed bookkeeping, other size classes and live objects are
additional. Sender and owner idle periods can retain partial batches/inboxes.

## Single-core controls

Five alternating trials, 1,048,576 operations/bursts each, CPU 3. The controls
use the retained production runner/UV/DPDK-prefix improvements on both sides.
The lane named `fast` here is the diagnostic's **ordinary awaitable + owning
fast runner**, not a change to fast-awaitable cancellation semantics.

The default-capacity comparison is recorded separately from the earlier
512-vs-512 control:

- [Default 256 vs legacy 512](single-core-default/summary.md),
  [rows](single-core-default/results.csv), [commands](single-core-default/environment.json).
- [512 vs 512 control](single-core/summary.md), [rows](single-core/results.csv).

For the selected default, real io_uring NOP time changes by -1.8% in Clang and
+0.7% in GCC. DPDK single-packet time changes from 66.19 to 68.96 ns in Clang
(+4.2%, about 2.8 ns), and 60.56 to 61.25 ns in GCC (+1.1%). Full-burst
results favor the candidate in this run but vary noticeably across control
runs; they do not establish a universal single-core improvement.

DPDK uses the software `net_ring` PMD with hot mbufs. No physical NIC, DMA,
network interface change, or wire-throughput measurement is involved.

## Validation and reproduction

- Desktop Clang C++14: 104 smoke tests passed with the default identity policy,
  including normal/no-exception paths, legacy-policy controls and real io_uring
  runtime checks. Installed core/io_uring package-consumer tests passed.
- Identity allocator, awaitable lifecycle and ordinary/fast pipeline lifecycle
  contracts passed ASan+UBSan and TSan. DPDK contracts passed normal,
  no-exception and ASan+UBSan checks.
- ARM64 GCC Debug: nine identity/legacy lifecycle and rejection tests passed.
  The focused final allocator contract also passed in Debug and `-O3 -DNDEBUG`
  with `-Wall -Wextra -Wpedantic -Werror`. GCC 14 at `-O1 -Werror` reports an existing
  `static_stack`/`inplace_t` maybe-uninitialized warning; ARM checks use the
  normal Debug configuration, and desktop Clang sanitizer checks use `-O1`.
- TLA+: 4,749 distinct states, no invariant failures. Removing either the
  close-quiescence guard or the admission-generation check produces a
  `Protected` invariant failure. This is an abstract ownership/lifetime model,
  not proof of the C++ memory model or progress under a stalled publisher.

The full Clang suite exposed an existing io_uring negative-test diagnostic
ordering issue: a throwing destructor also fails `is_nothrow_move_constructible`.
Checking the destructor contract first makes the intended diagnostic portable;
both exception modes then pass. No adapter runtime behavior changed.

Logs, sanitizer script, layout probe and final header hashes are in [tests](tests/).
The portable [capacity driver](../../../compare_identity_pool.py) accepts
source/output paths and CPU IDs. Both benchmark variants use real copied
header directories, avoiding relative-include escape through directory symlinks.
The [single-core driver](single-core-default/compare.py) uses the desktop's
existing diagnostic source/DPDK installation paths, which must be adapted on
another machine. Raw benchmark timings do not include sanitizer instrumentation.
