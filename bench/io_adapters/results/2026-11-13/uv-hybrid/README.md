# Bounded idle polling: selecting a workload-specific window

For this 3700X's same-L3 ordinary-awaitable/fast-runner workload, a **2 us idle
window** is a useful opt-in starting point when high-rate activity alternates
with idle gaps. It approaches continuous polling's 500k/s p99 and returns to
approximately blocking-mode CPU use between bursts. It is not a CPU-saving
policy during continuous activity, and it is not a universal platform default.

Keep blocking mode for sparse traffic. Do not promote the M1's 2 us short-run
result: longer measurements showed worse latency/CPU tradeoffs than continuous
polling and a high-load regression. Production executor defaults stay unchanged.

## Implementation and measurement

Measured revision `b849e5fb7d231db8aed34c2f9defc9e202e9b9e3`. `hybrid-N` is an
experimental driver in `bench/io_adapters/uv_handoff.cpp`; N is nanoseconds.
After each `uv_run` return, the owner reads `uv_metrics_info().events` and a
monotonic clock. Processing a backend event renews the idle window. The owner
uses `UV_RUN_NOWAIT` while the window remains open and `UV_RUN_ONCE` after it
expires. Native async notifications continue to wake the blocking loop. No
queue, memory-order, admission, close protocol or per-task callback changes
were made. Metrics/clock reads are an additional hybrid-path cost included in
the comparison. The implementation requires libuv >= 1.45.0 for hybrid mode.

The window is measured from the end of a turn that processed a backend event;
it is not a hard spin-time or CPU cap. Ongoing activity can renew it forever.
It does not ensure a quick first wakeup after a long idle gap. Other handles,
timer-only activity, energy consumption and mixed-I/O fairness were not tested.

Both machines used libuv 1.53.0 and matching recorded source hashes. Desktop:
Ryzen 7 3700X, CachyOS Linux 7.3.3-1-cachyos, Clang 23.1.4, owner CPU 3 and
producer CPU 2 (different physical cores sharing L3). Mac: M1 Max, macOS
15.3.2, Apple Clang 16.0.0, unpinned, AC at every recorded sweep boundary.
No priority, power or governor settings changed. The desktop host snapshot
was collected **after** the sweeps; it is not continuous telemetry.

Capacity 1024, callback budget 16, work 128, production adaptive retry backoff,
ordinary awaitable + owning fast runner. Warmup is 4,096 events per process.
Variants share one binary within each sweep and rotate/reverse their order.
Tables report medians of individual process quantiles, not pooled quantiles.
Paired wins are descriptive, not significance tests. CPU 100% is one CPU's
worth of time; the producer busy-waits even between bursts.

## Desktop: screen, refine, then confirm

The initial 262,144-event screen compared blocking, 0.5/2/8 us windows and
polling across 500k/s, 2M/s and bursts (five trials each). The 0.5 us window
added CPU cost without improving 500k/s p99. The 2 us and 8 us windows approached
polling latency while both slept between bursts.

A six-trial 500k/s refinement compared blocking, 0.75/1/1.5/2 us and polling.
The 1.5–2 us region approached polling latency. The 0.75 us option showed a
smaller benefit at lower CPU cost, so it was retained for longer confirmation.
The 1 us option used more CPU than 0.75 us with similar p99 in this refinement.
This is a sampled neighborhood, not proof of a globally optimal threshold.

Confirmation used 1,000,000 events/process and eight trials per variant/case,
covering both rotated and reversed positions for all four variants:

| Workload | Mode | e2e p99, us | CPU % | Lower p99 than blocking |
|---|---|---:|---:|---:|
| 500k/s | blocking | 3.681 | 154.147 | — |
| 500k/s | hybrid 0.75 us | 3.466 | 178.481 | 8/8 |
| 500k/s | hybrid 2 us | 1.526 | 199.471 | 8/8 |
| 500k/s | poll | 1.473 | 199.647 | 8/8 |
| 2M/s | blocking | 3.302 | 198.947 | — |
| 2M/s | hybrid 0.75 us | 2.920 | 199.199 | 7/8 |
| 2M/s | hybrid 2 us | 2.489 | 199.180 | 6/8 |
| 2M/s | poll | 3.013 | 199.620 | 6/8 |
| burst8192 | blocking | 3594.661 | 117.853 | — |
| burst8192 | hybrid 0.75 us | 3720.501 | 118.466 | 3/8 |
| burst8192 | hybrid 2 us | 3571.834 | 117.734 | 4/8 |
| burst8192 | poll | 3708.520 | 199.642 | 2/8 |

At 500k/s the 2 us window's p99 range was 1.468–1.568 us, versus
3.597–3.703 us blocking and 1.427–1.487 us polling. The hybrid median is about
0.053 us above polling. The 0.75 us option's modest long-run benefit costs
24.334 CPU percentage points, so it is not the preferred starting point here.

The burst schedule releases 8,192 tasks every 20 ms. Hybrid 2 us uses about
81.908 fewer CPU percentage points than polling, with p99 similar to blocking.
This is the reason to retain the hybrid candidate. Burst p99 includes draining
the scheduled batch; it does not isolate the first event's wakeup latency.

The 2M/s hybrid-2-us p99 ranged from 1.802 to **819.767 us**. Its favorable
median does not remove tail risk. Earlier polling measurements also showed
p99.9 regressions; these new medians do not supersede those observations.

## Sparse traffic rejects a universal window

A separate 100k/s test used 100,000 events/process and eight trials on desktop:

| Mode | e2e p99, us | CPU % |
|---|---:|---:|
| blocking | 3.550 | 122.857 |
| hybrid 0.75 us | 3.623 | 133.837 |
| hybrid 2 us | 3.639 | 144.119 |
| poll | 1.516 | 199.627 |

With arrivals 10 us apart, a 2 us post-event window generally expires before
the next arrival. It adds polling work while still requiring a wakeup. The
observed CPU/latency tradeoff favors blocking for this sparse case unless the
application explicitly wants continuous polling's latency at its CPU cost.

## M1: short-run promise did not survive confirmation

The Mac screen used 262,144 events/process, five trials and blocking,
2/8/16 us hybrid windows, and polling. Every hybrid window beat blocking's
500k/s p99 in 5/5 trials. The 2 us window was carried into a six-trial,
1,000,000-event confirmation alongside blocking and polling:

| Workload | Mode | e2e p99, us | CPU % | Lower p99 than blocking |
|---|---|---:|---:|---:|
| 500k/s | blocking | 42.916 | 137.805 | — |
| 500k/s | hybrid 2 us | 32.354 | 150.749 | 4/6 |
| 500k/s | poll | 24.270 | 148.431 | 6/6 |
| 2M/s | blocking | 6688.562 | 189.912 | — |
| 2M/s | hybrid 2 us | 13232.000 | 196.304 | 2/6 |
| 2M/s | poll | 5975.459 | 195.899 | 4/6 |
| burst8192 | blocking | 5868.750 | 116.840 | — |
| burst8192 | hybrid 2 us | 6364.229 | 117.553 | 2/6 |
| burst8192 | poll | 6160.646 | 124.570 | 4/6 |

Hybrid 2 us had a 500k/s p99 range of 25.792–116.042 us. At 2M/s it worsened
median e2e p99 and used more CPU than blocking. At 100k/s (six trials,
100,000 events each), hybrid p99 was 28.146 us at 118.789% CPU versus polling's
23.562 us at 118.810% and blocking's 43.979 us at 109.894%. There is insufficient
benefit to recommend this hybrid implementation on the Mac. The 8/16 us
windows were screened, not long-run validated; no claim is made for them.

## Retained evidence and usage

All 386 measured processes completed the probe's completion/checksum,
timestamp-order and close checks. CSV keys/counts and matching source hashes
were verified after transfer. Every hybrid process recorded both blocking
and nonblocking turns. `process-turns.json` counts whole-process `uv_run`
calls including setup/warmup/teardown; it does not prove which phase slept.
Bounds, legacy CLI, cold/burst wakeup and raw/normal-runner smoke checks also
passed locally. These are not sanitizer or production deployment tests.

| Sweep | Rows | Results | Conditions | Process turn counts |
|---|---:|---|---|---|
| Desktop screen | 75 | [CSV](desktop-screen/results.csv) | [Metadata](desktop-screen/environment.json) | [Counts](desktop-screen/process-turns.json) |
| Desktop refine | 36 | [CSV](desktop-refine/results.csv) | [Metadata](desktop-refine/environment.json) | [Counts](desktop-refine/process-turns.json) |
| Desktop confirm | 96 | [CSV](desktop-confirm/results.csv) | [Metadata](desktop-confirm/environment.json) | [Counts](desktop-confirm/process-turns.json) |
| Desktop sparse | 32 | [CSV](desktop-sparse/results.csv) | [Metadata](desktop-sparse/environment.json) | [Counts](desktop-sparse/process-turns.json) |
| Mac screen | 75 | [CSV](mac-screen/results.csv) | [Metadata](mac-screen/environment.json) | [Counts](mac-screen/process-turns.json) |
| Mac confirm | 54 | [CSV](mac-confirm/results.csv) | [Metadata](mac-confirm/environment.json) | [Counts](mac-confirm/process-turns.json) |
| Mac sparse | 18 | [CSV](mac-sparse/results.csv) | [Metadata](mac-sparse/environment.json) | [Counts](mac-sparse/process-turns.json) |

Each directory also contains `summary.txt` with ranges and paired comparisons.
[Bundle hashes](manifest.json), [desktop post-run snapshot](desktop-host-after.json)
and [Mac power snapshots](mac-power.json) are retained. Original binaries,
logs and bundles remain on both hosts. Desktop root:
`/home/nathan/ff-hybrid-20261113-vPmhS9`. Mac roots:
`build/remote-hybrid-20261113-204652`,
`build/remote-hybrid-confirm-20261113-204813`,
`build/remote-hybrid-sparse-20261113-205026`.

Example comparison for the Linux candidate:

```sh
CXX=clang++ python3 bench/io_adapters/compare_backoff.py --policies adaptive \
  --loop-policies blocking hybrid-2000 poll --profiles steady500k burst8192 \
  --modes ordinary-fast --trials 6 --owner-cpu 3 --producer-cpu 2
```

The CPU numbers are topology-specific; select distinct physical cores on the
target host. The 2 us policy remains a benchmark option and an application
driver candidate, not a new mandatory executor API or default.
