# Ryzen 7 3700X: blocking versus polling UV loops

At 500k events/s, polling lowered planned-arrival p99 in all six paired trials
for both raw tasks and ordinary awaitable + fast runner, in all three CPU
placements. It consumed approximately two cores instead of 1.52–1.64 cores.
At 2M/s, p99, p99.9 and throughput tradeoffs depended on placement; some tails
regressed. Keep polling opt-in. Existing executor defaults remain unchanged.

## Conditions and validation

Revision `0b39b1b85b1d1674d945cc23e91865e8c6fe6222`, CachyOS Linux
`7.3.3-1-cachyos`, Clang 23.1.4, libuv 1.53.0, C++14 `-O3 -DNDEBUG`.
The recorded source hashes match the [M1 comparison](../m1-loop-policy/README.md).
Compiler, hardware, OS and placement differ, so cross-host timing differences
do not isolate eventfd versus kqueue.

Six alternating trials per case, 4,096 warmup and 1,000,000 measured events
per process, work 128, capacity 1024, callback budget 16, adaptive retry
backoff. Both loop policies share one binary per placement. `blocking` uses
`UV_RUN_DEFAULT`; `poll` repeatedly calls `UV_RUN_NOWAIT` with a CPU relax.
No production source, priority, governor, boost or isolation setting changed.

The placements ran sequentially, with alternating policy and case order inside
each placement. CPU 3 owns the loop; producer CPU 2 shares its L3, whereas
producer CPU 4 uses the other L3. These are distinct physical cores, not SMT
siblings. The third placement leaves both threads unpinned. Placement order
was not interleaved, so compare policies within each placement first.

The governor was `powersave`, boost enabled, at all recorded boundaries.
Temperature sensor snapshots ranged from 27.25 to 51.50 C across the sweep;
they are not continuous frequency/thermal measurements. [Host metadata](host.json)
retains topology, load averages and per-placement boundary readings.

All 144 measured processes completed with exit code zero and passed the
probe's completion/checksum, timestamp-order and close checks. CSVs contain
one measured observation for every requested trial/case/policy. The runs
are performance/functional probes, not sanitizer tests.

## 500k events/s

Values are medians of per-process quantiles and CPU percentages. CPU 100%
represents one CPU's worth of time; CPU use is not energy consumption.

| Placement | Mode | Blocking p99, us | Poll p99, us | Poll wins | CPU %, blocking → poll |
|---|---|---:|---:|---:|---:|
| same L3 | raw | 3.481 | 1.324 | 6/6 | 152.355 → 199.554 |
| same L3 | ordinary-fast | 3.635 | 1.826 | 6/6 | 153.702 → 199.542 |
| cross L3 | raw | 4.927 | 2.034 | 6/6 | 159.561 → 199.614 |
| cross L3 | ordinary-fast | 5.200 | 2.569 | 6/6 | 163.986 → 199.610 |
| unpinned | raw | 4.239 | 1.559 | 6/6 | 153.467 → 199.432 |
| unpinned | ordinary-fast | 4.982 | 1.952 | 6/6 | 160.209 → 199.419 |

Queue-full retries were zero in every 500k trial. The same-L3 ordinary-fast
gain saves about 1.81 us of p99 at a cost of 45.84 CPU percentage points.
Same-L3 p99.9 does not improve consistently: ordinary-fast median p99.9
changes from 41.390 to 46.971 us, with polling winning only 3/6 trials.

## 2M events/s

| Placement | Mode | Blocking p99, us | Poll p99, us | Poll p99 wins | Blocking p99.9, us | Poll p99.9, us |
|---|---|---:|---:|---:|---:|---:|
| same L3 | raw | 2.847 | 2.166 | 6/6 | 220.600 | 291.089 |
| same L3 | ordinary-fast | 4.188 | 3.806 | 5/6 | 274.236 | 314.937 |
| cross L3 | raw | 4.585 | 4.935 | 1/6 | 12.837 | 9.338 |
| cross L3 | ordinary-fast | 153177.832 | 129333.600 | 4/6 | 154557.139 | 130516.612 |
| unpinned | raw | 3.498 | 2.309 | 5/6 | 35.612 | 37.448 |
| unpinned | ordinary-fast | 56642.378 | 3.653 | 5/6 | 57160.243 | 37.691 |

Same-L3 raw polling worsens p99.9 in **all six trials**, even though p99 improves
in all six. Same-L3 ordinary-fast p99.9 worsens in five of six. Cross-L3 raw
p99 worsens in five of six. These are observed regressions, not a claim about
their kernel-level cause; this sweep did not record scheduler traces.

Cross-L3 ordinary-fast often cannot sustain the offered rate: median
completion throughput is 1.533M/s blocking and 1.586M/s polling. Median
actual-submission p99 is 4.530/5.250 us, while submission-lag p99 is
153.175/129.331 ms. The large planned-arrival tails describe a producer
falling behind its offered schedule, not a 129 ms task service time.
Queue-full retries remain zero in this cross-L3 case. Quantiles are independent
and must not be added or subtracted for event-level attribution.

Unpinned medians hide major outliers. Raw polling p99 ranges from 1.129 us to
33.859 ms. Ordinary-fast polling p99 ranges from 2.509 us to 87.825 ms, versus
6.131 us to 178.797 ms blocking. The favorable polling medians do not establish
stable tails or guaranteed 2M/s capacity. Per-process CPU use is near 200% in
most 2M cases; polling does not eliminate overload or OS scheduling.

These are synthetic cross-thread tasks, not NIC/file I/O latency. Other loop
handles' fairness and power consumption were not measured. Win counts are
descriptive, not significance tests; p99 values are not pooled quantiles.

## Reproduction and retained data

```sh
CXX=clang++ python3 bench/io_adapters/compare_backoff.py \
  --policies adaptive --loop-policies blocking poll --trials 6 \
  --owner-cpu 3 --producer-cpu 2
```

Use producer CPU 4 for cross-L3, or omit both CPU options for unpinned.

| Placement | Observations | Summary | Build/environment |
|---|---|---|---|
| same L3 | [CSV](same-l3/results.csv) | [Full ranges](same-l3/summary.txt) | [Metadata](same-l3/environment.json) |
| cross L3 | [CSV](cross-l3/results.csv) | [Full ranges](cross-l3/summary.txt) | [Metadata](cross-l3/environment.json) |
| unpinned | [CSV](unpinned/results.csv) | [Full ranges](unpinned/summary.txt) | [Metadata](unpinned/environment.json) |

All source, native binaries, logs and diagnostic bundles remain on desktop in
`/home/nathan/ff-loop-linux-20261113-jH7SiZ`. The experiment used a new isolated
checkout and retained existing files.
