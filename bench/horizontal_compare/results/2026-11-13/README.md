# Asio / FF matched workload battle — 2026-11-13

FF's fast awaitable + fast runner path outperformed Asio C++20 coroutines in
all eight common-backend throughput comparisons (two hosts × two chain
lengths × two wait policies), with six of six trials favoring FF in every
comparison. The advantage is workload-specific: hand-written callbacks are
usually faster, ordinary FF runners do not win every case, and macOS tail
latency does not follow the throughput ranking.

This is **successful in-memory asynchronous execution**, not network latency,
cancellation/error equivalence, or io_uring/DPDK performance. FF's fastest lane
borrows an immutable blueprint; the ordinary-awaitable fast-runner lane owns
a blueprint handle. They are separately labeled throughout.

## Method and provenance

- Benchmark source: `6811a5d6a8efa560858288c3f2373eae638bca3f`; both hosts had no
  tracked diff. Asio 1.39.0 at `190a8d8daf1cc62909b4e0c72ff491ba264ca98e`.
- All lanes in one C++20 `-O3 -DNDEBUG` binary per host, exceptions enabled;
  native library allocator/pool defaults, no allocator substitutions.
- Six independent processes per case; 4,096 warmup and 131,072 measured
  requests per process, work=128 unsigned arithmetic iterations **per step**,
  capacity=1,024. 88 cases and 528 processes per host; **1,056 total**.
- One producer and one consumer. 3700X pinned producer=2, owner=3, same L3,
  powersave governor unchanged. Mac unpinned, AC power confirmed before and
  after; no QoS, power or scheduler settings changed.
- `chain`: common Asio backend, one initial consumer envelope followed by
  1 or 4 explicitly posted work steps. Real `asio::awaitable` / `co_spawn`
  compared with callbacks and fresh FF runners over reusable blueprints.
- `handoff`: producer starts a request, one queued work step and completion
  on the consumer. Asio vs UV, no coroutine lane because co_spawn would add
  an initial envelope. UV capacity=1,024, drain budget=16. Both backends obey
  the same external logical-request credit limit.
- `blocking` and `poll` are compared separately on each host. Neither OS uses
  a different policy from its opponent within a comparison. The Linux-only
  hybrid candidate was correctness-tested, not included in this measurement
  matrix. Production executor defaults are unchanged.
- Variants rotate/reverse within matched blocks; block order is shuffled
  with a recorded seed. Every request checks checksum, step order/count,
  envelope count, consumer-thread execution, exactly one completion and time
  ordering; a consumer fence drains final callback cleanup.

See the [full methodology and one-command runner](../../README.md).
Tables show **medians across six processes**; brackets show min–max between
processes, not confidence intervals. Latency quantiles are not pooled across
processes. CPU includes the busy producer; 200% means two busy cores.

| Host | Compiler | libuv | OS |
|---|---|---|---|
| 3700X / Linux | clang version 23.1.4 | 1.53.0 | Linux-7.3.3-1-cachyos-x86_64-with-glibc2.44 |
| M1 Max / macOS | Apple clang version 16.0.0 (clang-1600.0.26.6) | 1.53.0 | macOS-15.3.2-arm64-arm-64bit |

## Common Asio backend: saturated throughput

Throughput in **million completed requests/s**. Saturation fills the bounded
queue, so its latency is not an unloaded service-latency measurement.

| Host | Work steps | Policy | Asio coroutine | FF fast/fast view | FF advantage | FF vs raw callback |
|---|---:|---|---:|---:|---:|---:|
| 3700X / Linux | 1 | blocking | 1.259 | 1.992 | +58.2% | -7.7% |
| 3700X / Linux | 1 | poll | 1.042 | 1.910 | +83.3% | +0.2% |
| 3700X / Linux | 4 | blocking | 0.540 | 0.653 | +21.0% | -10.4% |
| 3700X / Linux | 4 | poll | 0.571 | 0.738 | +29.4% | -10.2% |
| M1 Max / macOS | 1 | blocking | 1.708 | 2.227 | +30.4% | -16.6% |
| M1 Max / macOS | 1 | poll | 1.417 | 2.155 | +52.1% | -13.3% |
| M1 Max / macOS | 4 | blocking | 0.701 | 0.784 | +11.9% | -13.9% |
| M1 Max / macOS | 4 | poll | 0.634 | 0.725 | +14.4% | -9.5% |

The Linux single-step polling difference from raw callbacks is only +0.2%;
treat that as a tie within variability, not evidence that adding a flow makes
callbacks faster. The other median gaps from raw are roughly 8–17%. These
are ratios of medians; six-trial wins compare matching trial numbers.

All lanes under **blocking**, including variability:

| Host | Lane | 1 step, M/s [min–max] | 4 steps, M/s [min–max] |
|---|---|---:|---:|
| 3700X / Linux | Asio raw callback | 2.157 [1.778–2.242] | 0.729 [0.708–0.872] |
| 3700X / Linux | Erased callback | 1.968 [1.854–2.136] | 0.695 [0.692–0.697] |
| 3700X / Linux | Asio coroutine | 1.259 [1.149–1.330] | 0.540 [0.537–0.610] |
| 3700X / Linux | FF normal | 1.615 [1.456–1.750] | 0.522 [0.519–0.643] |
| 3700X / Linux | FF fast runner + ordinary awaitable | 1.990 [1.723–2.019] | 0.615 [0.610–0.754] |
| 3700X / Linux | FF fast runner + fast awaitable (view) | 1.992 [1.672–2.057] | 0.653 [0.651–0.803] |
| M1 Max / macOS | Asio raw callback | 2.671 [2.624–2.717] | 0.911 [0.886–0.914] |
| M1 Max / macOS | Erased callback | 2.362 [2.338–2.446] | 0.827 [0.825–0.832] |
| M1 Max / macOS | Asio coroutine | 1.708 [1.707–1.716] | 0.701 [0.698–0.704] |
| M1 Max / macOS | FF normal | 1.734 [1.662–1.751] | 0.624 [0.617–0.629] |
| M1 Max / macOS | FF fast runner + ordinary awaitable | 2.158 [2.078–2.180] | 0.758 [0.741–0.764] |
| M1 Max / macOS | FF fast runner + fast awaitable (view) | 2.227 [2.190–2.267] | 0.784 [0.775–0.792] |

The normal FF runner loses to Asio coroutines in the four-step blocking
case on both hosts. The result supports the fast-path design, not a claim
that every FF configuration is faster. Blueprint ownership, state layout,
allocation/recycling, reference counting and native coroutine bookkeeping
all differ; this experiment does not isolate their individual contributions.

## Common backend: single-step latency at 500k requests/s

This is the **chain** contract (initial envelope plus one work step). End-to-end
latency includes lateness from planned arrivals. Stable completion near 500k/s
means the offered load was sustained; it is not maximum throughput.

| Host | Policy | Lane | e2e p99, us [min–max] | e2e p99.9, us [min–max] | CPU % |
|---|---|---|---:|---:|---:|
| 3700X / Linux | blocking | Asio raw callback | 3.489 [3.443–3.560] | 5.048 [4.584–5.942] | 152.3 |
| 3700X / Linux | blocking | Asio coroutine | 4.264 [4.185–4.893] | 6.062 [5.820–6.948] | 163.1 |
| 3700X / Linux | blocking | FF fast runner + ordinary awaitable | 3.708 [3.654–3.872] | 5.373 [5.163–5.816] | 157.0 |
| 3700X / Linux | blocking | FF fast runner + fast awaitable (view) | 3.609 [3.498–3.781] | 5.405 [5.005–5.748] | 156.1 |
| 3700X / Linux | poll | Asio raw callback | 1.137 [0.940–1.326] | 3.090 [2.443–3.513] | 199.6 |
| 3700X / Linux | poll | Asio coroutine | 1.671 [1.502–1.707] | 3.893 [2.915–4.079] | 199.6 |
| 3700X / Linux | poll | FF fast runner + ordinary awaitable | 1.411 [1.164–1.619] | 3.417 [2.747–4.732] | 199.6 |
| 3700X / Linux | poll | FF fast runner + fast awaitable (view) | 1.369 [1.178–1.576] | 3.346 [2.937–3.981] | 199.6 |
| M1 Max / macOS | blocking | Asio raw callback | 44.438 [43.458–45.416] | 63.959 [56.917–66.625] | 139.0 |
| M1 Max / macOS | blocking | Asio coroutine | 45.001 [43.750–46.375] | 87.396 [81.417–94.417] | 149.8 |
| M1 Max / macOS | blocking | FF fast runner + ordinary awaitable | 45.751 [43.708–56.125] | 86.645 [63.083–115.041] | 145.0 |
| M1 Max / macOS | blocking | FF fast runner + fast awaitable (view) | 45.020 [44.083–47.833] | 79.166 [65.916–145.833] | 142.6 |
| M1 Max / macOS | poll | Asio raw callback | 20.062 [11.666–44.084] | 80.958 [60.791–114.000] | 192.0 |
| M1 Max / macOS | poll | Asio coroutine | 47.729 [23.625–85.000] | 224.938 [84.292–728.750] | 187.3 |
| M1 Max / macOS | poll | FF fast runner + ordinary awaitable | 40.209 [14.917–57.917] | 119.021 [93.708–189.542] | 187.0 |
| M1 Max / macOS | poll | FF fast runner + fast awaitable (view) | 40.271 [15.625–73.500] | 115.375 [81.833–242.875] | 194.0 |

On Linux, polling lowers these fixed-load p99s while consuming close to two
cores. On Mac, blocking single-step p99s cluster around 43–46 us across raw
callbacks, coroutines and fast FF flows. Polling is variable: for the common
Asio backend, FF fast/fast p99 falls from 45.020 to 40.271 us, but p99.9 rises
from 79.166 to 115.375 us and CPU rises from 142.6% to 194.0%. **There is no
universal Mac tail-latency win from polling.**

The shared blocking tails and wait-policy sensitivity support a substantial
waiting/wakeup/scheduling contribution. They do not prove that every tail is
an OS scheduler delay: no scheduler trace was collected in this battle, and
queueing, backend behavior, allocation, cache traffic and the unpinned host
remain possible contributors. Four-step Mac tails are especially variable;
for example raw/blocking p99 spans 68.833–2578.125 us and ordinary-awaitable
FF fast-runner/poll p99 spans 45.042–16224.041 us. All such trials are retained.

## One cross-thread handoff: Asio vs UV

Selected raw and fastest-flow lanes below; ordinary/erased/normal lanes are
also present in the complete CSV. Throughput and latency columns are from
**separate saturated and steady500k processes**, respectively.

| Host | Policy | Backend / lane | Saturated M/s [min–max] | 500k/s e2e p99, us [min–max] | 500k/s p99.9, us | 500k/s CPU % |
|---|---|---|---:|---:|---:|---:|
| 3700X / Linux | blocking | asio / raw | 2.345 [2.222–2.892] | 3.721 [3.510–3.748] | 4.867 | 151.1 |
| 3700X / Linux | blocking | asio / FF fast/fast view | 2.290 [2.190–2.646] | 3.678 [3.592–3.835] | 5.006 | 152.9 |
| 3700X / Linux | blocking | uv / raw | 2.390 [2.345–2.853] | 3.651 [3.471–3.674] | 4.611 | 153.3 |
| 3700X / Linux | blocking | uv / FF fast/fast view | 2.377 [2.226–2.788] | 3.739 [3.712–3.775] | 4.797 | 154.0 |
| 3700X / Linux | poll | asio / raw | 2.829 [2.299–2.881] | 1.103 [1.020–1.152] | 2.530 | 199.6 |
| 3700X / Linux | poll | asio / FF fast/fast view | 2.625 [2.242–2.669] | 1.319 [1.222–1.381] | 2.428 | 199.6 |
| 3700X / Linux | poll | uv / raw | 2.860 [2.418–2.939] | 1.300 [1.282–1.418] | 3.296 | 199.6 |
| 3700X / Linux | poll | uv / FF fast/fast view | 2.756 [2.321–2.837] | 1.366 [1.312–1.482] | 3.270 | 199.6 |
| M1 Max / macOS | blocking | asio / raw | 3.039 [3.013–3.092] | 43.541 [43.375–45.125] | 57.125 | 127.9 |
| M1 Max / macOS | blocking | asio / FF fast/fast view | 2.797 [2.757–2.878] | 43.417 [43.208–46.916] | 58.125 | 132.9 |
| M1 Max / macOS | blocking | uv / raw | 3.079 [2.944–3.314] | 42.666 [42.166–43.542] | 54.500 | 132.9 |
| M1 Max / macOS | blocking | uv / FF fast/fast view | 3.155 [2.751–3.181] | 42.354 [42.250–44.000] | 53.646 | 135.5 |
| M1 Max / macOS | poll | asio / raw | 2.934 [2.821–2.947] | 30.021 [18.542–96.416] | 113.000 | 192.1 |
| M1 Max / macOS | poll | asio / FF fast/fast view | 2.640 [2.591–2.659] | 21.312 [11.125–33.166] | 76.230 | 189.4 |
| M1 Max / macOS | poll | uv / raw | 3.298 [2.975–3.318] | 24.250 [17.417–30.041] | 45.605 | 146.4 |
| M1 Max / macOS | poll | uv / FF fast/fast view | 3.015 [2.354–3.178] | 22.937 [18.709–26.083] | 45.791 | 147.3 |

UV and Asio are in the same throughput range for this workload. The Mac UV
fast-flow blocking median slightly exceeds its raw median, with overlapping
trial ranges; do not interpret that as negative abstraction overhead. The
cross-backend table measures the whole executor/flow path, whereas the chain
table holds the backend fixed. Their rates and latencies are not interchangeable.

## Reproduce and inspect

```sh
# Same-L3 3700X setup used here (choose valid CPU IDs for another machine):
python3 bench/horizontal_compare/battle.py --owner-cpu 3 --producer-cpu 2
# M1 Max, native ARM64, AC power:
python3 bench/horizontal_compare/battle.py
```

- [Desktop process data](desktop/results.csv), [summary](desktop/summary.csv), [environment and commands](desktop/environment.json).
- [Mac process data](mac/results.csv), [summary](mac/summary.csv), [environment and commands](mac/environment.json).
- [Artifact hashes and retained raw-log locations](manifest.json).
- [Validation record](validation.json): final-source ASan+UBSan matrix passed
  66 processes on desktop; UBSan passed 66 on the ARM64 development host.
  The local ARM64 ASan runtime could not initialize its shadow allocator, so
  memory-safety validation ran on desktop after performance collection.
- All 1,056 measured rows were checked for expected case/trial counts, matching
  source/dependency revisions, finite ordered quantiles and credit bounds;
  every stored summary was independently recomputed from process data.

The legacy serial benchmark tables remain historical snapshots. They use
different work, serial draining and dependency selection, so these figures
are a new comparison rather than an apples-to-apples improvement percentage
over the old tables.
