# Adapter measurements, 2026-11-13

Seven warmed measurements per lane, 1,048,576 operations per UV/io_uring run
and 1,048,576 bursts per DPDK run. Tables show medians. These are aggregate
throughput costs, not individual request latency. See the
[harness methodology](../../README.md) for workloads, validation and commands.

UV compares the mutex header from `4e4daf5` with the current ticket/MPSC header.
All other source files are shared. Source hashes match between the Pi and
desktop; hashes, build commands and CPU information are in each environment
JSON. No executor/awaitable implementation was changed during this measurement.

## Hosts and conditions

| | Raspberry Pi 4 | Ryzen 7 3700X |
|---|---|---|
| CPU | 4 × Cortex-A72, up to 1.8 GHz | 8 cores / 16 threads, boost enabled |
| OS/kernel | Debian 13, Linux 6.18.39+rpt-rpi-v8 | CachyOS, Linux 7.3.3-1-cachyos |
| Main compiler | Clang 19.1.7 | Clang 23.1.4 |
| libuv | 1.50.0 | 1.53.0 |
| liburing | 2.9 | 2.15 |
| DPDK | 24.11.4 | 25.11.3 |
| Owner / producers | CPU 0 / 1, 2, 3 | CPU 3 / 4, 5, 6 (distinct physical cores) |
| Governor | ondemand | powersave, boost enabled |

The Pi was about 35–36 °C at the endpoints; no governor or isolation settings
were changed. Frequency readings are snapshots, not continuous frequency
measurements. The desktop software differs, so cross-host ratios do not isolate
CPU architecture. All old/new or raw/wrapped comparisons within a host use the
same compiler and dependencies. Package dependencies on both hosts were
extracted into temporary directories without system package installation.

## UV: ns/task, lower is better

| Producers / callback budget | Pi mutex | Pi current | 3700X mutex | 3700X current |
|---|---:|---:|---:|---:|
| Owner-inline / 16 | 114.595 | 120.515 | 19.141 | 18.032 |
| 1 / 16 | 861.144 | 337.895 | 202.092 | 118.646 |
| 1 / 64 | 746.974 | 338.350 | 161.665 | 143.187 |
| 3 / 16 | 1014.432 | 468.118 | 535.248 | 178.000 |
| 3 / 64 | 787.298 | 392.475 | 407.621 | 182.471 |

For this saturated tiny-task workload, cross-thread median throughput improves
2.01–2.55× on the Pi and 1.13–3.01× on the desktop. Owner-inline costs about 5%
more on the Pi and about 6% less on the desktop. Budget 64 is not universally
faster than 16. These measurements do not assess fairness to other loop I/O.

The Pi's 3-producer/budget-64 current lane ranges from 381 to 1008 ns/task;
do not read its median as a tail-latency or stability guarantee. All ranges and
raw retry/close data are retained. Close is verified, but seven teardown samples
are insufficient to characterize contended shutdown latency.

An exploratory short run used a truly single-threaded process for inline
dispatch and saw roughly 51 vs 96 ns on the Pi. That result is **not** the final
inline comparison: glibc/libstdc++ single-thread shortcuts were active. The
published harness starts/joins a thread before every process's warmup.

## io_uring: ns/NOP, batch 32

| Path | Pi | 3700X |
|---|---:|---:|
| Native liburing | 125.780 | 85.538 |
| Attached context callback | 185.576 | 92.637 |
| Normal awaitable runner | 905.240 | 211.780 |
| Fast awaitable runner | 489.932 | 154.582 |

On the desktop the context lane is about 7 ns/NOP above raw, and the fast
flow lane about 69 ns/NOP above raw. These differences are comparisons between
complete compiled paths, not independently additive component timings.
Normal and fast runners are freshly constructed for each asynchronous request.

The userspace-only staged probe gives 2.438 / 13.578 / 132.318 / 76.453 ns on
the desktop for raw/context/normal/fast. This suggests that flow lifecycle cost
is a more promising profiling target than the attachment routing alone; it
does not identify allocation or a particular atomic as the bottleneck.
Batch-1 and batch-256 results are retained in the full summaries.

## DPDK: ns/burst, 32 packets per burst

This is **actual native DPDK software ring PMD RX/TX**, with 64-byte hot mbufs
recycled on one core. It is not a NIC, DMA or wire-throughput measurement.

| Path | Pi Clang 19 | 3700X Clang 23 | 3700X GCC 16.2.1 |
|---|---:|---:|---:|
| Native RX/TX | 703.155 | 276.861 | 268.753 |
| RAII packet batch | 773.907 | 294.095 | 286.454 |
| Context callback | 785.878 | 286.057 | 302.959 |
| Normal awaitable runner | 1482.755 | 335.614 | 361.020 |
| Fast awaitable runner | 1172.494 | 263.358 | 314.413 |

Clang's desktop fast lane is slightly faster than the raw lane. A second
compiler reverses that ordering: GCC gives about 46 ns extra per 32-packet
burst, or 1.43 ns/packet amortized. The result is compiler-sensitive; it does
not establish that framework abstraction has intrinsically negative overhead.
Generated-code differences have not been attributed to a specific optimization
or layout mechanism. No performance-specific source changes were made to favor
either result.

Batch size matters: with **one packet per flow**, the desktop raw/fast costs
are 11.045/80.528 ns under Clang and 12.104/69.581 ns under GCC. The favorable
32-packet amortization cannot be applied to one-flow-per-packet designs.

Every DPDK run verified packet count/checksum, empty final ring and return of
all 4095 mempool objects, followed by port and EAL cleanup. No physical device
was configured, bound or used. There is still no physical-NIC validation.

## Artifacts

- [Pi medians and ranges](pi4/summary.md), [UV CSV](pi4/uv.csv),
  [io_uring CSV](pi4/uring.csv), [DPDK CSV](pi4/dpdk.csv),
  [build/environment](pi4/environment.json), [DPDK build](pi4/dpdk-environment.json).
- [3700X medians and ranges](3700x/summary.md), [UV CSV](3700x/uv.csv),
  [io_uring CSV](3700x/uring.csv), [DPDK CSV](3700x/dpdk.csv),
  [build/environment](3700x/environment.json), [DPDK build](3700x/dpdk-environment.json).
- [3700X GCC DPDK medians and ranges](3700x-gcc-dpdk/summary.md),
  [CSV](3700x-gcc-dpdk/dpdk.csv), [build](3700x-gcc-dpdk/environment.json).

Reproduce the desktop matrix using owner CPU 3 and first producer CPU 4; use
DPDK `--cpu 3`. The GCC diagnostic only changes `CXX=c++` and the output
directory. Full CSVs include every repeat; no outliers were discarded.
