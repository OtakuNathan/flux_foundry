# UV, io_uring and DPDK adapter costs

These Linux probes exercise the **actual public FF adapters**. The older
`bench/event_loop` executable has its own experimental libuv adapter and does
not measure `executor/uv_executor.h`.

For a one-command macOS/Linux suite using the current adapters, see
[`python3 bench/run.py --uv on`](../README.md#one-command-macos--linux-suite).
The UV C++ drivers accept `-1` for each CPU argument to disable affinity;
the Linux comparison scripts below retain their pinned defaults.

## Run

Requirements: C++14 compiler, Python 3, pthreads, pkg-config, libuv and liburing
development packages. DPDK additionally needs its development package and the
`net_ring` and `mempool_ring` software PMDs. No physical NIC is used.

```sh
CXX=clang++ python3 bench/io_adapters/run.py \
  --baseline 4e4daf5 --count 1048576 --repeats 7
CXX=clang++ python3 bench/io_adapters/run_dpdk.py \
  --bursts 1048576 --repeats 7 --cpu 0
python3 bench/io_adapters/summarize.py build/io-perf build/io-perf-dpdk
```

`run.py` defaults to owner CPU 0 and producer CPUs 1, 2, 3. Override with
`--owner-cpu` and `--first-producer-cpu`; choose distinct physical cores when
possible. `--out` selects an output directory. The exported baseline can also
be supplied with `--baseline-header` on a machine without Git history.
The baseline replaces **only** the UV header; all other headers and benchmark
source are identical. The script writes the old header to an isolated overlay
and leaves the working tree intact.

Both scripts use `-std=c++14 -O3 -DNDEBUG -pthread`, exceptions enabled, no
sanitizers, and no controlled-layout or function-alignment flags. They honor
`CXX` and `EXTRA_CXXFLAGS`. `run_dpdk.py` also uses DPDK's pkg-config architecture
flags. For packages extracted outside the system, set `PKG_CONFIG_PATH`,
`PKG_CONFIG_SYSROOT_DIR` and `LD_LIBRARY_PATH`. `run.py` deliberately uses the
system libuv pkg-config search path alongside an optional extracted liburing.

Run these sequentially, without concurrent builds or other benchmarks.
Each lane warms up before timing; UV old/new order alternates, and io_uring/DPDK
lane order rotates across repeats. Frequency governors, turbo, interrupts and
the rest of the host remain unchanged. The probes are pinned, not CPU-isolated.
Reported ranges are the full observed min/max, not confidence intervals.

## What is timed

### UV

Queue capacity 1024, callback budgets 16/64, tiny SBO tasks, owner-inline or
one/three producer threads. A full queue retries the **same unconsumed task**
and yields; CSV records rejection counts. There are no drops. Owner-side
per-producer sequence checks verify delivery and FIFO order. Thread creation,
executor/loop initialization and final thread joins are outside the throughput
window. Start release, producer scheduling, queueing, wakeups and execution up
to the last completed task are included. `ns_per_op` is inverse aggregate
throughput, **not per-task latency**. Saturating retries are part of this load.

Every process starts and joins a thread before warmup, including owner-inline
cases. This prevents glibc/libstdc++ single-thread mutex/shared_ptr shortcuts
from making inline results unrepresentative of an already multithreaded host.

`close_us` measures from the final task's completion timestamp through close
request and return from `uv_run`, including any remaining sender ticket and
callback cleanup. It is one sample per run, not a cancellation/close-contention
latency distribution. Every case verifies native handle/loop closure.

### io_uring

Compare raw liburing, attached context callbacks, normal runners and fast
runners. The latter two build one shared blueprint before timing and create a
fresh one-shot runner **per request**. Runner creation, framework ownership and
completion cleanup are timed. Ring/context initialization and detach are not.

`kernel` submits real `IORING_OP_NOP` requests with default ring flags, in
fill/submit/drain batches of 1/32/256. Submission handles short counts; CQEs are
advanced before invoking FF completion. All lanes use the same batch policy and
verify result, completion count and identifier checksum. There is no disk,
network, SQPOLL, IOPOLL, overlapping batch pipeline or cross-thread continuation.

`staged` only prepares userspace SQEs and routes tokens back after each batch;
it performs **no kernel I/O**. Its purpose is to separate userspace adapter/flow
cost from submission/completion syscalls. Do not report it as I/O throughput.

### DPDK

The native [ring PMD](https://doc.dpdk.org/guides-24.11/nics/pcap_ring.html)
backs one configured and started ethdev with an SP/SC software ring. EAL uses
`--no-huge --no-pci --no-shconf --no-telemetry`, 64 MiB, and one selected lcore.
The benchmark does not bind devices, configure hugepages or change interfaces.

Seed the ring with 1 or 32 real 64-byte mbufs, then repeatedly receive, inspect
the first byte and length, and transmit those same mbufs back to the ring.
Compare raw burst APIs, the RAII packet batch, context callbacks, normal runners
and fast runners. Every flow represents **one burst**, not one packet. Runner
creation and packet-batch ownership transfers are inside timing. Blueprint,
EAL, port, ring and mempool creation are outside it. All returned packets must
be accepted by the loopback TX; partial-TX performance is not measured.

Packet count/checksum and final ring emptiness/full mempool recovery are checked.
This is a single-core, hot-cache software-loopback measurement. It includes no
DMA, PCIe, real NIC, wire bandwidth, cross-core traffic, packet allocation per
burst, protocol parsing or realistic working set. `ns_per_packet` is simply
`ns_per_burst / burst_size` and is not individual packet latency.

## Results

The November 2026 snapshot and raw CSVs are under
[`results/2026-11-13`](results/2026-11-13/README.md).

### Diagnose the two independent dimensions

`fast` in the original matrix means **fast runner + the public ordinary
awaitable**, not a fast-awaitable blueprint. The runner does not rewrite node
types or their cancellation/lifetime contracts.

`diagnose.py` explicitly substitutes benchmark-only `fast_awaitable_base`
implementations, and independently selects owning or borrowed blueprint
storage. It generates sources under the output directory and preserves the
original public adapters and probes. `terminal` changes the contract to exactly
one backend terminal callback, with no controller-driven logical cancellation.
`view` requires the host blueprint to outlive all outstanding completions.

```sh
python3 bench/io_adapters/diagnose.py --backend uring \
  --reference build/io-perf --out build/diag-uring --cpu 3
python3 bench/io_adapters/diagnose.py --backend dpdk \
  --reference build/io-perf-dpdk --out build/diag-dpdk --cpu 3
python3 bench/io_adapters/trace_alloc.py --diagnostic build/diag-uring --cpu 3
python3 bench/io_adapters/trace_alloc.py --diagnostic build/diag-dpdk --cpu 3
```

The reference directories supply their original compile commands, so they must
belong to this checkout and host. Preserve the library environment used by the
original build. The allocation probe wraps direct executable calls to malloc,
aligned allocation and global new; it does not intercept internals of shared
libraries. Its timings are instrumentation artifacts, not performance results.

[3700X diagnosis and ownership/cancellation tradeoffs](results/2026-11-13/diagnosis/README.md).

### Hardware counters and sampling

`perf_probe.py` builds profiling copies of the diagnostic variants and uses
perf's control/ack file descriptors to enable events only after warmup. Ring,
blueprint, EAL and port initialization are excluded. The process is pinned by
the existing harness; counters are per-process, user-space only (`:u`). Kernel
I/O work is excluded from these counters, even in the real-NOP lane.

```sh
python3 bench/io_adapters/perf_probe.py --perf /path/to/perf \
  --uring build/diag-uring --dpdk build/diag-dpdk \
  --out build/perf-adapters --cpu 3
python3 bench/io_adapters/summarize_perf.py build/perf-adapters
```

Defaults: three rotated stat trials of 16,777,216 operations/bursts per lane;
cycles, instructions, branches and branch misses counted together. The summary
rejects unavailable or substantially multiplexed events. Selected lanes also
record user-space cycles at 997 Hz with DWARF user-only call chains. Debug information is
added without disabling optimization or forcing frame pointers. Raw `.data`
files remain in the output directory; self and inclusive symbol reports are
generated separately. Sampling periods can skid, so annotation next to an
atomic instruction is not an exact per-instruction cycle attribution.

The profiling copies introduce control hooks and select one DPDK lane/batch per
process. This can change generated-code layout; their timings are not silently
substituted for the original benchmark snapshot. Control acknowledgements add
a small bounded overhead around the enabled interval. See the
[3700X perf results](results/2026-11-13/perf/README.md).

On Zen 2, `perf_atomics.py --perf /path/to/perf --reference build/perf-adapters`
adds grouped counters for retired locked instructions and store-to-load
interlocks. These event names are CPU-specific; their local perf descriptions
are saved with the measurements. `--record-only` on `perf_probe.py` resumes
sampling from completed stat runs without rebuilding or repeating counters.

The resulting two-line factory optimization is measured separately in the
[runner move comparison](results/2026-11-13/runner-move/README.md).

The subsequent [packet batch comparison](results/2026-11-13/batch-prefix/README.md)
holds capacity at 32 and varies the number of received packets. It measures
copying only the owned prefix when moving a batch, with the runner optimization
applied to both versions. `compare_batch_prefix.py` records the header hashes
and alternates before/after order across seven trials.

[Further screening](results/2026-11-13/next-squeeze/README.md) tests three
additional candidates against that optimized baseline. None established a
consistent latency gain, so their patches remain unapplied.

The subsequent [cross-core UV study](results/2026-11-13/cross-core/README.md)
removes a redundant internal `shared_ptr` copy from dispatch, with lifetime
contracts and sanitizer coverage. It includes saturated multi-producer
throughput and a separate scheduled-arrival latency probe using the production
executor. For the latter, build `uv_handoff.cpp` with C++14, `-O3 -pthread`, the
repository include directory and `-luv`, then run for example:

```sh
./uv_handoff ordinary-fast steady500k 32768 128 16 3 2
```

Arguments after the profile are sample count, synthetic work iterations,
callback budget, owner CPU and producer CPU. Capacity is 1024. `ordinary-fast`
uses an ordinary awaitable and an owning fast runner; `raw` and `normal` are
also supported. The output distinguishes warmup from measured records. Choose
CPU IDs according to the host's actual cache topology.

[Shared-pool follow-up](results/2026-11-13/producer-perf/README.md) profiles the
remaining producer-side cost and tests small batch transfers between TLS caches
and the shared pool. Cross-core throughput improves, but single-core DPDK
tradeoffs left those generic allocator variants unapplied; the added
cross-thread reclamation contracts are retained.

The subsequent [identity-pool study](results/2026-11-13/identity-pool/README.md)
routes batches to the original allocator's TLS inbox without allocation headers
or executor involvement. It compares capacities 128/256/512/1024 and retains
256 with a batch of 16 as the latency/space compromise. The awaitable policy is
enabled by default; `FLUX_FOUNDRY_AWAITABLE_IDENTITY_POOL=0` selects the legacy
policy. To reproduce a capacity comparison on suitable CPU IDs:

```sh
python3 bench/io_adapters/compare_identity_pool.py --out build/identity-compare \
  --trials 7 --owner 3 --producers 2 4 --capacities 128 256 512 1024
```
