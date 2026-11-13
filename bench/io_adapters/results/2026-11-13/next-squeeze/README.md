# Further optimization screening

Three additional candidates were screened after the runner-factory and
packet-prefix optimizations. None is applied to the production headers:
these runs did not establish a consistent latency improvement worth retaining.
The patches preserve the experiments for later workloads.

## Method

Ryzen 3700X, CPU 3 affinity, Clang 23.1.4 and GCC 16.2.1, C++14 `-O3`.
Each case runs three warmed trials of 1,048,576 requests or bursts, alternating
before/after order. These are screening trials, not a statistical equivalence
test or proof that the candidates cannot help other workloads. No candidate
met the threshold for a longer confirmation run and production validation.

Both versions include the prior runner-factory fix. DPDK comparisons also
include the retained packet-prefix optimization, with capacity fixed at 32
and occupancy 1, 4, 16 and 32. They use the native software net_ring PMD, with
packet-count/checksum and pool-return checks. No NIC configuration is changed.
io_uring measures both userspace-only staged routing and real kernel NOPs at
batch sizes 32, 256 and 1024, checking completion counts and checksums.

There are 624 timing rows: 240 for each DPDK candidate and 144 for io_uring.
Each directory retains compile commands, header hashes, raw timings and
medians with the full observed ranges. Separate candidates are compared to
their own contemporaneous baseline; do not compare absolute times across
the three experiments as if they were paired trials.

## Findings

### Avoid value-initialization of unused batch slots

[Candidate patch](ctor.patch): replace the explicitly defaulted batch default
constructor with a user-provided empty constructor. The count still initializes
to zero, but value-initialization no longer zeroes unused pointer slots.

There is no consistent fast-runner improvement. At occupancy 1, Clang changes
61.86 → 63.36 ns/burst (+2.4%) and GCC 58.47 → 58.10 (−0.6%). At occupancy 4,
both regress slightly. The apparent occupancy-16 gains occur in a more variable
region and do not establish a general benefit. The normal receive path already
default-initializes its batch without zeroing the unused array.

[Full summary](ctor/summary.md), [timings](ctor/results.csv),
[environment](ctor/environment.json). Decision: retain the existing constructor.

### Explicit block copy for the owned pointer prefix

[Candidate patch](memcpy.patch): replace the two move loops with `std::memcpy`
over the initialized prefix. Self-move remains guarded by the existing check.

The batch-only Clang measurements are essentially unchanged, as are most
fast-runner measurements. GCC fast runner at occupancy 4 regresses from
68.44 → 73.06 ns/burst (+6.8%); occupancy 1 and 32 are essentially unchanged.
This does not justify replacing the existing loop.

[Full summary](memcpy/summary.md), [timings](memcpy/results.csv),
[environment](memcpy/environment.json). Decision: retain the existing move loops.

### Narrow io_uring slot links

[Candidate patch](slots.patch): narrow the private free-list link from `size_t`
to `uint32_t`, with explicit conversion at assignments. The maximum permitted
capacity and sentinel are 65536, so they fit. On this 64-bit host the slot layout
shrinks from 32 to 24 bytes: 25% less slot-array storage, not 25% less total
adapter or process memory.

GCC staged context-only routing improves about 2.7–4.5%; Clang is effectively
unchanged. On real kernel NOPs, ordinary awaitable + fast runner changes:

| Compiler | Batch 32 | Batch 256 | Batch 1024 |
|---|---:|---:|---:|
| Clang | −1.3% | +0.5% | −0.2% |
| GCC | +0.2% | +0.3% | +0.2% |

This provides a possible memory-footprint optimization, but no convincing
end-to-end latency win in these tests. It remains an unapplied candidate.

[Full summary](slots/summary.md), [timings](slots/results.csv),
[environment](slots/environment.json).

## Reproduction and limits

The DPDK candidates use [compare_batch_prefix.py](../../../compare_batch_prefix.py)
with the saved baseline and candidate headers, `--runner after-flow_runner.h`,
`--references build/io-perf-dpdk build/io-perf-dpdk-gcc`,
`--count 1048576 --repeats 3`. Native extracted-library environment is the same
as the preceding packet-prefix experiment. Outputs remain in
`/tmp/ff-io-perf-vFZ6HVwW/build/next-squeeze-{ctor,memcpy}` on the desktop.

[compare_slots.py](compare_slots.py) is the exact one-off io_uring driver; run
from that scratch checkout with `baseline-uring.h`, `compact-uring.h` and the
existing `build/batch-prefix-final/after/flow` overlay. It creates a fresh
`build/next-squeeze-slots` output directory. The recorded compile commands and
header hashes make the tested variants explicit.

Production lifecycle code is unchanged. Existing submission-lifetime holds,
backend references and cancellation state operations were inspected but not
removed: they protect different lifetime and race conditions. This screening
does not prove their implementation is optimal. The earlier experimental
terminal-only awaitable and borrowed-blueprint measurements show scope under
stricter contracts; they remain separate choices from runner type.

These results cover small single-core operations, not cross-core queueing,
real network traffic, p99/p999 latency or overload. Those workloads remain the
next useful way to identify a material architectural bottleneck.
