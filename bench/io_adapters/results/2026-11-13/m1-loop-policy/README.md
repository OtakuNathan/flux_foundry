# M1 Max UV loop policy, 2026-11-13

Continuous nonblocking polling reduced median planned-arrival p99 by 42–44%
at 500k events/s, at increased CPU cost. At 2M/s neither polling nor a larger
callback budget produced a consistent winner. These measurements support an
application-specific loop policy experiment; they do not justify changing the
executor's defaults.

Measured revision `b9a5824efa3156cf8b39532f62bd42a54433704f`, Apple M1 Max,
macOS 15.3.2, Apple Clang 16.0.0, libuv 1.53.0. Threads were unpinned. Both
sweeps started and ended on AC power. `pmset` reported no recorded thermal or
performance warning; it also reported no recorded CPU power status. These
snapshots do not establish constant frequency or absence of throttling.
[Power snapshots](power.json) are retained. No power, QoS or affinity settings
were changed.

Each process has 4,096 warmup events and 1,000,000 measured events, work 128,
queue capacity 1024 and production adaptive retry backoff. Six interleaved
trials use the same binary within each sweep. `blocking` drives
`UV_RUN_DEFAULT`; `poll` repeatedly drives `UV_RUN_NOWAIT` with a CPU relax
between iterations. The executor, async sends and callback drain path are
shared. Completion, checksum, timestamp ordering and close checks passed in
all 120 measured processes. Production headers were not modified.

The first sweep alternates the two variants and reverses case order. The
second rotates six variants so each occupies every position once. Results
are descriptive; paired win counts are not significance tests. Tables show
medians of per-process p99 values, not a quantile pooled across processes.

## Loop policy at budget 16

```sh
python3 bench/io_adapters/compare_backoff.py --policies adaptive \
  --loop-policies blocking poll --trials 6
```

| Case | Blocking e2e p99, us | Poll e2e p99, us | Poll wins | CPU %, blocking → poll |
|---|---:|---:|---:|---:|
| raw / 500k | 42.770 | 23.896 | 6/6 | 127.347 → 144.597 |
| ordinary awaitable + fast runner / 500k | 42.541 | 24.708 | 6/6 | 138.197 → 149.367 |
| raw / 2M | 1105.833 | 1328.542 | 3/6 | 193.913 → 194.246 |
| ordinary awaitable + fast runner / 2M | 4339.729 | 4664.125 | 4/6 | 190.748 → 195.139 |

At 500k, raw blocking/poll p99 ranges were 42.375–44.708 / 21.792–25.208 us.
Ordinary-fast ranges were 42.167–43.708 / 21.291–26.459 us. Queue-full retries
were zero in every 500k trial. The measured CPU increases are 17.250 and
11.170 percentage points, respectively; 100% represents one CPU's worth of
time. This measures process CPU, not energy use.

At 2M the trial variability is substantial, and a majority of paired wins can
coexist with a worse median across independent trials. Keep both statistics.
For ordinary-fast, median submission-lag p99 is 4.053 / 4.264 ms while
actual-submission p99 is 0.406 / 0.507 ms. Planned-arrival p99 includes lateness
accumulated before submission; these separate quantiles cannot be added or
subtracted to attribute individual events. Throughput near 2M/s is constrained
by the offered schedule and does not establish maximum capacity.

[48 observations](loop/results.csv), [full ranges](loop/summary.txt),
[build commands and source hashes](loop/environment.json).

## Loop policy × callback budget at 2M/s

```sh
python3 bench/io_adapters/compare_backoff.py --policies adaptive \
  --loop-policies blocking poll --budgets 16 64 256 \
  --profiles overload2m --trials 6
```

| Budget / loop | raw e2e p99, us | ordinary-fast e2e p99, us |
|---|---:|---:|
| 16 / blocking | 2282.062 | 5270.583 |
| 16 / poll | 2193.563 | 5363.438 |
| 64 / blocking | 2204.792 | 4534.749 |
| 64 / poll | 1826.626 | 5153.500 |
| 256 / blocking | 726.292 | 4952.166 |
| 256 / poll | 1630.583 | 4961.062 |

The raw budget-256/blocking median looks favorable, but it beats
budget-16/blocking in only 3/6 paired trials and ranges from 47.167 to
4202.334 us. Ordinary-fast budget-64/blocking beats the baseline in 4/6 trials,
with p99 ranging from 3391.834 to 7210.750 us. No configuration wins reliably
across these workloads. This sweep does not reproduce a strong, universal
budget-256 advantage from earlier measurements.

[72 observations](budget/results.csv), [full ranges and paired counts](budget/summary.txt),
[build commands and source hashes](budget/environment.json).

Both sweeps measure synthetic cross-thread task handoff, not NIC or file I/O
latency. They do not measure fairness to other handles. Polling reduces one
source of blocking but still uses backend system calls and remains subject to
OS scheduling. The earlier System Trace established runnable waiting on the
blocking path; this experiment does not establish that every remaining tail
event has the same cause.

All native logs, binaries and bundles remain on the Mac under
`build/remote-loop-20261113-192131` and
`build/remote-loop-budget-20261113-192337`. The copied bundles were SHA-256
verified during transfer through the existing SSH/tmux session.
