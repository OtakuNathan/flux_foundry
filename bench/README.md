# Microbenchmark Snapshots

This document keeps the microbenchmark material out of the root `README`.

## One-command macOS / Linux suite

From the repository root, with Python 3 and a C++14 compiler:

```sh
python3 bench/run.py
```

On a Mac, install the Xcode command-line tools if `c++` is unavailable
(`xcode-select --install`). To include the UV throughput and awaitable handoff
probes, install libuv and pkg-config, then require that suite explicitly:

```sh
brew install libuv pkg-config
python3 bench/run.py --uv on
```

The default `--uv auto` includes UV when pkg-config finds libuv, otherwise it
prints a skip notice. Nothing is downloaded or installed by the runner. It uses
`CXX` (default `c++`) and optional `EXTRA_CXXFLAGS`; Homebrew include/library
paths come from pkg-config. Run natively on Apple Silicon, on AC power with
other heavy work stopped. Threads are unpinned on both platforms; results do
not identify performance/efficiency-core placement or match the pinned Ryzen
topology experiments.

The runner builds release binaries, executes three process trials, and saves
`summary.txt`, `results.csv`, `environment.json` (revision, compiler, commands),
and raw stdout/stderr under a new `build/bench-TIMESTAMP/` directory. Use
`--trials 7` for more repetitions, `--out /path/to/new-directory` to choose the
destination, or `--skip-flow --trials 1 --samples 4096 --count 65536` for a smoke
run. A compilation/runtime failure stops the suite and keeps its logs.

Measurements have separate meanings:

- Flow with/without exceptions: ns/op, including inline-completing awaitables.
  These are not I/O latencies. Raw flow `p95` describes round averages.
- Simple executor: one producer/consumer, scheduled 500k/s and 2M/s, raw tasks,
  fast awaitable + fast runner, and ordinary awaitable + ordinary runner.
- Optional UV saturation: tiny tasks, one or three producers, completed tasks/s.
- Optional UV handoff: raw tasks, ordinary awaitable + fast runner, and ordinary
  awaitable + ordinary runner, with the same two scheduled arrival rates.

Scheduled tests use work=128 and queue capacity=1024; UV drain budget is 16.
Their throughput is constrained by offered load and is **not** a saturation
measurement. `e2e_p99_us` starts at planned arrival, including producer lateness;
`actual_p99_us` starts at submission. Reported p99 summaries are medians of each
trial's p99, not pooled quantiles. Timing instrumentation is included. The UV
drivers warm up before measurement; the simple executor probe starts with cold
pools. The default allocator policy is compiled as-is; this is not an old/new
allocator comparison. The suite does not exercise io_uring, DPDK, or a real NIC.

Share `summary.txt` and `environment.json` for an initial comparison; retain the
CSV and raw logs for investigating variability.

### Diagnose latency spikes

```sh
python3 bench/run.py --uv on --skip-flow --trials 3 --samples 1000000 --trace
```

This exports the scheduled probes' **existing** seven timestamps after timing,
validation, and the FIFO fence. It adds no per-event clock reads or trace buffers.
UV warmup is excluded. After each process exits, Python compresses each raw CSV
and builds the following files in the result directory:

- `*.samples.csv.gz`: every event's timestamps in nanoseconds relative to that
  case's planned origin, with its event ID. Keep these for detailed analysis.
- `*.timeline.csv`: 1 ms windows, planned/submitted/started/completed counts,
  remaining planned submissions and unfinished submitted events at window end.
  These are logical operation counts, not executor queue occupancy. Duration
  maxima are grouped by the event's **submission** window; count columns use
  each transition's own time. Empty windows are retained.
- `*.spikes.csv`: the 16 largest end-to-end latency events, their timestamps and
  per-event latency breakdown. These are maxima, not representative p99 samples.
- `trace-summary.txt`: per-case maxima and definitions. The ordinary
  `summary.txt` still contains throughput and p99 statistics.

For an individual event, `e2e = submit_lag + pre_dispatch + dispatch_to_begin +
service`. `pre_dispatch` includes producer-side runner/awaitable preparation.
`dispatch_to_begin` includes enqueueing/retries and the wait until the callback
begins; it is not a pure queue residence measurement. `service` runs from callback
entry through the receiver's completion timestamp, excluding subsequent cleanup.
`dispatch_call` and `submit_call` can overlap consumer execution and must not be
added to that decomposition.

`producer_gap` is time from the previous submit return to the next submit start;
it includes intentional arrival pacing. `consumer_gap` is time from the previous
completion timestamp to the next callback start; it includes callback cleanup,
loop work, idle time and scheduling delays. A gap alone does not prove a thread
was descheduled, blocked on allocation, or migrated between CPU types.

Start with windows where unfinished work grows: compare submission counts,
completion counts, submission lag, dispatch duration and callback service time.
Then inspect individual spikes and nearby events in the raw trace. OS-level
profiling is a separate follow-up once the affected path is identified.

Large runs create substantial trace data and take extra time to process. Writing
and compression happen outside measurement, but diagnostic code layout and
between-case activity can still affect results; keep an untraced control run.
For sharing, send `diagnostics.tar.gz`: the runner bundles `summary.txt`,
`results.csv`, `environment.json`, `trace-summary.txt` and the small `*.timeline.csv` /
`*.spikes.csv` files. Full sample traces stay outside this small bundle.

### Compare full-queue backoff parameters

```sh
python3 bench/io_adapters/compare_backoff.py
```

This focused UV experiment reuses `utility/back_off.h` directly. It compares
`backoff_strategy<>` (16 spin stages, then `std::this_thread::yield()`) with
`backoff_strategy<SIZE_MAX>` (effectively spin-only within a bounded benchmark).
Both retain the same exponential spin loop and default `max_loop=1024`.
The spin case requires a 64-bit step counter and verifies that no scheduler
yield was called. ARM's `yield` instruction remains part of both spin loops;
the changed threshold controls the C++ scheduler-yield call.

Only the benchmark's no-drop retry wrapper around `uv_executor::try_dispatch`
changes policy. Production headers/defaults, UV callback budget, allocator,
other internal backoffs and simple executor remain unchanged. This experiment
therefore tests one possible source of long dispatch calls, not all possible
causes of a stalled producer or consumer.

One binary contains both policies. The script runs five paired trials of raw
tasks and ordinary awaitable + fast runner, at 500k/s and 2M/s, alternating
policy/case order. Each case has one million measured events and its own warmup.
Both policies count rejected dispatch attempts and scheduler-yield calls on
the producer thread; these counters add instrumentation relative to the plain
probe. A zero yield count means this mechanism was not exercised in that run.

Process CPU time is sampled only at the measurement boundaries. CPU percentage
includes both threads: 100% means one CPU's worth of time, 200% means two. The
interval includes the 2 ms arrival lead-in and final fence, but excludes setup,
quantile sorting and trace output. It measures total workload CPU cost rather
than isolated retry cost. Lower tail latency with higher CPU use is a tradeoff,
not automatically a better production default. This comparison does not prove
whether a thread was preempted or migrated between CPU types.

Use `--modes normal` to test the ordinary runner separately,
`--profiles overload2m` to focus on pressure, or `--trace` for event timelines.
The default omits trace output to reduce between-case activity. Optional trace
compression runs after each pair. `CXX`, `EXTRA_CXXFLAGS` and pkg-config work as
in the main suite; threads are unpinned unless Linux CPU arguments are supplied.
For a smoke run: `--trials 2 --samples 8192`.

Send the generated `build/backoff-TIMESTAMP/backoff-diagnostics.tar.gz`, which
contains environment details, per-trial results and the comparison summary
(plus small timeline/spike files when `--trace` is enabled).

To hold the default backoff fixed and compare only UV callback budgets:

```sh
python3 bench/io_adapters/compare_backoff.py --policies adaptive --budgets 16 64 256 --trials 6
```

With three budgets, six trials cover every budget in every position twice,
using rotated and reversed variant order. Other parameters and the binary stay
the same. Larger budgets let a callback process more queued tasks before
returning to the event loop; this can also delay other handles, which this
single-handle workload does not measure. Report CPU cost and tail variability
alongside throughput. This experiment does not directly observe OS scheduling;
the package name remains `backoff-diagnostics.tar.gz` for both comparisons.

For actual UV executor old/new comparisons and io_uring/DPDK awaitable costs,
see [`bench/io_adapters/README.md`](io_adapters/README.md).

For the standalone `flux_foundry` vs Asio harness and collection scripts, see [`bench/horizontal_compare/README.md`](horizontal_compare/README.md).

For new before/after flow-probe comparisons, use the same benchmark sources and
compiler flags on both revisions. Default builds retain natural stack and function
placement, so layout-related slowdowns remain observable. Pin the timed process,
interleave old/new runs, and compare multiple process runs and stack placements.
A repeatable layout-related slowdown counts as a regression for that workload,
even when the hot-path instruction sequence is unchanged.

For diagnosis, configure CMake with `-DFLUX_FOUNDRY_PERF_CONTROLLED_LAYOUT=ON`.
This adds aligned sink/caller anchors (Clang/AArch64 also realigns the calling
frame) and, with GNU/Clang, `-falign-functions=64`. For manual GNU/Clang builds of
either probe, use `-DFLUX_FOUNDRY_PERF_CONTROLLED_LAYOUT=1 -falign-functions=64`.
Apply these settings equally to both revisions. This mode helps isolate layout
effects; passing it alone does not establish that default builds have no
regression. Historical snapshots below retain their original settings.

## Const blueprint layout investigation (2026-11-11)

Comparing `22a9b4c` with `0137f6b`, the original natural-layout C++14 probe on
Cortex-A72 with Clang 19 showed a 4.7–7.5% slowdown in
`runner.fast_awaitable.async4` across four initial-stack placements. The untimed
`make_sync_20_bp` helper shrank by 16 bytes, moving downstream functions.
In a temporary assembly control, restoring those addresses reduced the candidate
from 871.8 to 823.6 ns/op; branch misses per thousand instructions fell from
11.06 to 7.92. All measured hot functions then matched the baseline's addresses
and instruction bytes. This supports a code-placement effect associated with
branch prediction; the exact predictor mechanism remains unidentified. The
assembly padding was only a diagnostic experiment and is not part of the library.

On Ryzen 7 3700X, C++14 comparisons with Clang 23 and GCC 16, with and without
exceptions, did not reproduce a similarly stable slowdown. These compiler
versions differ from the A72 setup, so this does not isolate CPU differences.
The separate single-step `async1` stack-position sensitivity affects both
revisions; its precise hardware mechanism remains unresolved. The feature is
retained with this workload-specific regression documented, rather than a claim
that every build has unchanged performance.

## Direct continuation construction (2026-11-11)

Compared with `b19c735`, runner helpers now return concrete closures and the
awaitable delegate constructs fresh callbacks in their final storage. An SBO
move-count probe drops from two payload moves to one. Existing erased-callback
transfers and replacement of occupied callbacks retain their previous paths.

Natural-layout C++14 probes used identical sources and flags on both revisions:
`-O3 -DNDEBUG -pthread`, plus `-fno-exceptions
-DFLUX_FOUNDRY_NO_EXCEPTION_STRICT=1 -fno-rtti -march=native -fstrict-aliasing`
for no-exception builds. Runs were pinned to CPU 3 with per-process ASLR disabled,
alternating old/new order and varying an environment variable's length to move
the initial stack. Each reported process result is the probe's seven-round median.
For Ryzen 7 3700X, medians of the first two process runs were:

| Compiler / mode | `runner.fast_awaitable.async4` before → after (ns/op) | Change |
|---|---:|---:|
| Clang 23 / exceptions | 91.73 → 72.50 | −21.0% |
| Clang 23 / no exceptions | 87.56 → 58.40 | −33.3% |
| GCC 16 / exceptions | 94.47 → 68.89 | −27.1% |
| GCC 16 / no exceptions | 88.62 → 67.56 | −23.8% |

On Cortex-A72 / Clang 19, two runs in opposite orders with different stack
placements gave 557.78 → 438.53 ns/op (−21.4%) with exceptions and
522.65 → 364.92 ns/op (−30.2%) without exceptions for the same case.

These awaitables complete synchronously in `submit()`: the results measure
framework overhead, not end-to-end asynchronous I/O speedups.

The full Clang 23 exception-enabled executable also showed layout-sensitive
changes in the unaffected `runner.sync.20nodes` case. At four environment-padding
lengths (0/16/32/48 bytes), before → after results were 31.36 → 36.80,
31.24 → 36.84, 34.62 → 57.60, and 67.37 → 36.07 ns/op. Isolating the three
purely synchronous cases produced byte-identical executables from both header
versions. In the full executable, the synchronous callback's instruction sequence
matches after address relocations are excluded, while code placement and the
main stack frame differ. This supports layout sensitivity; the precise hardware
mechanism is unresolved, and the slower placements still count as workload
regressions. The asynchronous four-step improvement persisted at all four
placements. No padding or forced alignment was added to the library.

## CUDA backend comparison (RTX 2080 Ti, 2026-11-05)

Matched Mandelbrot rendering on real hardware, Ryzen 7 3700X + RTX 2080 Ti:
completed images/s, medians of five trials, with exceptions enabled. Each image
includes CUDA resource creation, kernel execution, D2H, completion handling,
resource cleanup and sampled image validation. Native uses the same backend and
polling queue; FF paths add generated Portal invocations and a fresh fast
runner per operation.

| Image | In-flight window | Native callbacks | FF + DB concrete | FF + DB erased |
|---|---:|---:|---:|---:|
| 128×128 | 1 | 5,918 | 6,091 | 5,871 |
| 128×128 | 8 | 17,282 | 17,708 | 18,114 |
| 512×512 | 1 | 2,716 | 2,687 | 2,706 |
| 512×512 | 8 | 1,485 | 1,465 | 1,459 |

The matched paths are close in this workload; positive FF differences in some
trials do not establish a speedup. At 512×512/window 8, median paired throughput
differences versus Native are −3.23% concrete and −5.58% erased. Native itself
also slows at window 8, so that slowdown is not specific to FF. Native still
uses shared ownership and `std::function`; this measures incremental integration
cost on that baseline, not absolute overhead versus bare CUDA.

See [methodology and reproduction](cuda/README.md) and the
[full snapshot with raw CSV and validation log](cuda/results/rtx2080ti-20261105/README.md).
Separate device-only references exclude allocation, D2H and validation and are
not equivalent end-to-end workloads. Eight upstream typed-CUDA tests passed,
including event-record failure injection and concrete/erased/no-exception paths.

## 📊 Benchmark snapshots
Measured on `2026-03-07` (local machine):

- OS: `Darwin 24.3.0` (`arm64`)
- CPU: `Apple M1 Max` (`10` cores)
- Memory: `64 GB`
- Compiler: `clang++/c++ -std=c++14 -O3 -DNDEBUG`

Commands:

```bash
# flow perf (exception / no-exception)
c++ -std=c++14 -O3 -DNDEBUG -I. test/flow_perf.cpp -o /tmp/flow_perf_exc -pthread
c++ -std=c++14 -O3 -DNDEBUG -fno-exceptions -DFLUX_FOUNDRY_NO_EXCEPTION_STRICT=1 -fno-rtti -march=native -fstrict-aliasing -I. test/flow_perf_noexcept.cpp -o /tmp/flow_perf_noexc -pthread
/tmp/flow_perf_exc
/tmp/flow_perf_noexc

# flux vs asio horizontal compare
bash bench/horizontal_compare/run.sh
bash bench/horizontal_compare/run_noexcept.sh
```

> The snapshot tables below predate enforced runner consumption. Current
> `flow_perf` labels synchronous aggregator cases as `*.oneshot.*` and creates
> a runner per operation, so those rows intentionally include runner creation
> and are not directly comparable with the historical aggregator rows below.

### Flow perf matrix (`ns/op`, median of 3 rounds)

| Case | Exceptions ON | Exceptions OFF |
|---|---:|---:|
| `direct.loop20` | 5.56 | 5.49 |
| `runner.sync.20nodes` | 32.28 | 26.62 |
| `fast_runner.sync.20nodes` | 5.51 | 5.12 |
| `runner.awaitable.async4` | 324.53 | 288.54 |
| `fast_runner.awaitable.async4` | 122.40 | 125.35 |
| `runner.fast_awaitable.async4` | 173.07 | 134.65 |
| `fast_runner.fast_awaitable.async4` | 87.02 | 77.77 |
| `runner.awaitable.async1` | 92.10 | 78.51 |
| `fast_runner.awaitable.async1` | 32.23 | 32.71 |
| `runner.fast_awaitable.async1` | 45.28 | 34.40 |
| `fast_runner.fast_awaitable.async1` | 23.49 | 16.73 |
| `runner.when_all.2` | 155.28 | 138.79 |
| `fast_runner.when_all_fast.2` | 92.78 | 79.39 |
| `runner.when_all_fast.2` | 113.80 | 93.95 |
| `runner.when_any.2` | 141.46 | 120.33 |
| `fast_runner.when_any_fast.2` | 74.96 | 64.95 |
| `runner.when_any_fast.2` | 91.12 | 79.80 |

### Horizontal compare (exceptions ON, core matrix, `ns/op`)

| Case | ns/op |
|---|---:|
| `baseline.direct.loop20` | 5.39 |
| `sched.flux.via20.dispatch` | 221.83 |
| `sched.asio.dispatch20` | 137.18 |
| `sched.flux.via20.post` | 210.87 |
| `sched.asio.post20` | 548.29 |
| `full.flux.runner.async4.post` | 597.28 |
| `full.flux.fast_runner.async4.post` | 401.51 |
| `full.flux.runner.fast_awaitable.async4.post` | 474.11 |
| `full.flux.fast_runner.fast_awaitable.async4.post` | 406.96 |
| `full.asio.post.async4` | 145.72 |
| `full.flux.runner.when_all2.post` | 242.65 |
| `full.flux.fast_runner.when_all2.post` | 187.81 |
| `full.flux.fast_runner.when_all2.fastagg.post` | 181.66 |
| `full.asio.post.when_all2` | 138.96 |
| `full.flux.runner.when_any2.post` | 235.94 |
| `full.flux.fast_runner.when_any2.post` | 191.82 |
| `full.flux.fast_runner.when_any2.fastagg.post` | 168.12 |
| `full.asio.post.when_any2` | 136.17 |

### Horizontal compare (strict no-exception, expanded matrix excerpt, `ns/op`)

| Case | ns/op |
|---|---:|
| `baseline.direct.loop20` | 6.33 |
| `sched.flux.native.via20.inline` | 78.23 |
| `sched.flux.native.via20.dispatch` | 1332.06 |
| `sched.flux.on_asio.via20.dispatch` | 205.30 |
| `sched.asio.raw.dispatch20` | 130.37 |
| `sched.flux.on_asio.via20.post` | 193.08 |
| `sched.asio.raw.post20` | 533.65 |
| `full.flux.native.fast_runner.fast_awaitable.async4.inline` | 78.64 |
| `full.flux.native.fast_runner.fast_awaitable.async1.inline` | 20.17 |
| `full.flux.on_asio.fast_runner.fast_awaitable.async4.dispatch` | 229.79 |
| `full.asio.raw.dispatch.async4` | 79.82 |
| `full.asio.adapter.dispatch.async4` | 126.56 |
| `full.flux.on_asio.fast_runner.fast_awaitable.async4.post` | 321.33 |
| `full.asio.raw.post.async4` | 144.78 |
| `full.asio.adapter.post.async4` | 193.65 |
| `full.flux.on_asio.fast_runner.fast_awaitable.async1.dispatch` | 104.69 |
| `full.asio.raw.dispatch.async1` | 70.26 |
| `full.asio.adapter.dispatch.async1` | 83.75 |
| `full.flux.on_asio.fast_runner.fast_awaitable.async1.post` | 104.43 |
| `full.asio.raw.post.async1` | 70.60 |
| `full.asio.adapter.post.async1` | 84.45 |
| `full.flux.on_asio.fast_runner.when_all2.fastagg.post` | 162.32 |
| `full.asio.raw.post.when_all2` | 138.22 |
| `full.flux.on_asio.fast_runner.when_any2.fastagg.post` | 141.49 |
| `full.asio.raw.post.when_any2` | 137.60 |

### Real backend async overhead (strict no-exception, same host/toolchain, `ns/op`)

| Case | ns/op |
|---|---:|
| `real.flux.fast_runner.fast_awaitable.async4.backend` | 5497.95 |
| `real.asio.raw.async4.backend` | 4975.41 |
| `real.flux.fast_runner.fast_awaitable.async1.backend` | 5285.36 |
| `real.asio.raw.async1.backend` | 4991.35 |


### Windows snapshot (`2026-03-02`, separate local host)

- OS: `Windows 11 Home` (`10.0.26200`, `x64`)
- CPU: `AMD Ryzen 7 3700X` (`8` cores / `16` threads)
- Memory: `31.91 GB`
- Compiler: `clang++ 21.1.0` (`x86_64-pc-windows-msvc`)

Commands (PowerShell equivalent):

```bash
# flow perf (exception / no-exception)
clang++ -std=c++14 -O3 -DNDEBUG -I. test/flow_perf.cpp -o test/bin/flow_perf_exc.exe
clang++ -std=c++14 -O3 -DNDEBUG -fno-exceptions -DFLUX_FOUNDRY_NO_EXCEPTION_STRICT=1 -fno-rtti -march=native -fstrict-aliasing -I. test/flow_perf_noexcept.cpp -o test/bin/flow_perf_noexc.exe
.\test\bin\flow_perf_exc.exe
.\test\bin\flow_perf_noexc.exe

# flux vs asio horizontal compare
# set ASIO include path first, e.g. $env:ASIO_INCLUDE="C:\path\to\asio\include"
clang++ -std=c++14 -O3 -DNDEBUG -pthread -I. -I"$env:ASIO_INCLUDE" bench/horizontal_compare/compare_flux_asio.cpp -o test/bin/flux_asio_horizontal_compare.exe
# NOTE: Asio path uses RTTI internally (typeid), so do NOT pass -fno-rtti here.
clang++ -std=c++14 -O3 -DNDEBUG -fno-exceptions -DFLUX_FOUNDRY_NO_EXCEPTION_STRICT=1 -DASIO_NO_EXCEPTIONS=1 -DASIO_DISABLE_EXCEPTIONS=1 -march=native -fstrict-aliasing -pthread -I. -I"$env:ASIO_INCLUDE" bench/horizontal_compare/compare_flux_asio_noexcept.cpp -o test/bin/flux_asio_horizontal_compare_noexcept.exe
.\test\bin\flux_asio_horizontal_compare.exe
.\test\bin\flux_asio_horizontal_compare_noexcept.exe
```

Windows notes:

- Keep this as a host-specific snapshot; regenerate locally with the command block above.
- The macOS tables in this README are the currently maintained baseline.

### Flow perf matrix (Windows, `ns/op`, median of 7 rounds)

| Case | Exceptions ON | Exceptions OFF |
|---|---:|---:|
| `direct.loop20` | 6.14 | 5.93 |
| `runner.sync.20nodes` | 31.41 | 21.27 |
| `fast_runner.sync.20nodes` | 10.62 | 5.84 |
| `runner.awaitable.async4` | 351.80 | 313.17 |
| `fast_runner.awaitable.async4` | 160.62 | 118.47 |
| `runner.fast_awaitable.async4` | 201.00 | 155.88 |
| `fast_runner.fast_awaitable.async4` | 117.82 | 81.60 |
| `runner.awaitable.async1` | 90.78 | 79.88 |
| `fast_runner.awaitable.async1` | 41.35 | 27.25 |
| `runner.fast_awaitable.async1` | 51.02 | 37.62 |
| `fast_runner.fast_awaitable.async1` | 33.25 | 18.68 |
| `runner.when_all.2` | 202.62 | 187.85 |
| `fast_runner.when_all_fast.2` | 157.65 | 128.37 |
| `runner.when_all_fast.2` | 173.73 | 148.01 |
| `runner.when_any.2` | 178.62 | 159.00 |
| `fast_runner.when_any_fast.2` | 120.11 | 98.49 |
| `runner.when_any_fast.2` | 140.10 | 117.55 |

### Horizontal compare (Windows, exceptions ON, core matrix, `ns/op`)

| Case | ns/op |
|---|---:|
| `baseline.direct.loop20` | 6.07 |
| `sched.flux.via20.dispatch` | 1520.06 |
| `sched.asio.dispatch20` | 1467.20 |
| `sched.flux.via20.post` | 1490.82 |
| `sched.asio.post20` | 9605.10 |
| `full.flux.runner.async4.post` | 3181.42 |
| `full.flux.fast_runner.async4.post` | 3275.80 |
| `full.flux.runner.fast_awaitable.async4.post` | 3639.41 |
| `full.flux.fast_runner.fast_awaitable.async4.post` | 3424.16 |
| `full.asio.post.async4` | 2725.10 |
| `full.flux.runner.when_all2.post` | 1722.61 |
| `full.flux.fast_runner.when_all2.post` | 1848.91 |
| `full.flux.fast_runner.when_all2.fastagg.post` | 1771.33 |
| `full.asio.post.when_all2` | 1940.99 |
| `full.flux.runner.when_any2.post` | 1636.75 |
| `full.flux.fast_runner.when_any2.post` | 1652.85 |
| `full.flux.fast_runner.when_any2.fastagg.post` | 1551.59 |
| `full.asio.post.when_any2` | 1931.10 |

### Horizontal compare (Windows, strict no-exception, expanded matrix excerpt, `ns/op`)

| Case | ns/op |
|---|---:|
| `baseline.direct.loop20` | 5.89 |
| `sched.flux.native.via20.inline` | 72.86 |
| `sched.flux.native.via20.dispatch` | 404.06 |
| `sched.flux.on_asio.via20.dispatch` | 1424.66 |
| `sched.asio.raw.dispatch20` | 1425.52 |
| `sched.flux.on_asio.via20.post` | 1419.61 |
| `sched.asio.raw.post20` | 9533.45 |
| `full.flux.native.fast_runner.fast_awaitable.async4.inline` | 78.12 |
| `full.flux.native.fast_runner.fast_awaitable.async1.inline` | 25.83 |
| `full.flux.on_asio.fast_runner.fast_awaitable.async4.dispatch` | 1715.34 |
| `full.asio.raw.dispatch.async4` | 1474.33 |
| `full.asio.adapter.dispatch.async4` | 1552.46 |
| `full.flux.on_asio.fast_runner.fast_awaitable.async4.post` | 3098.98 |
| `full.asio.raw.post.async4` | 2892.24 |
| `full.asio.adapter.post.async4` | 2895.56 |
| `full.flux.on_asio.fast_runner.fast_awaitable.async1.dispatch` | 1458.18 |
| `full.asio.raw.dispatch.async1` | 1500.19 |
| `full.asio.adapter.dispatch.async1` | 1563.52 |
| `full.flux.on_asio.fast_runner.fast_awaitable.async1.post` | 1500.29 |
| `full.asio.raw.post.async1` | 1424.87 |
| `full.asio.adapter.post.async1` | 1458.67 |
| `full.flux.on_asio.fast_runner.when_all2.fastagg.post` | 1662.84 |
| `full.asio.raw.post.when_all2` | 1936.70 |
| `full.flux.on_asio.fast_runner.when_any2.fastagg.post` | 1511.55 |
| `full.asio.raw.post.when_any2` | 2132.18 |

### Real backend async overhead (Windows, strict no-exception, same host/toolchain, `ns/op`)

| Case | ns/op |
|---|---:|
| `real.flux.fast_runner.fast_awaitable.async4.backend` | 2531.64 |
| `real.asio.raw.async4.backend` | 2111.96 |
| `real.flux.fast_runner.fast_awaitable.async1.backend` | 2169.42 |
| `real.asio.raw.async1.backend` | 2265.26 |

### Linux snapshot (`2026-03-02`, same hardware as Windows snapshot)

- OS: `Ubuntu Linux` (same machine as Windows snapshot, switched OS)
- CPU: `AMD Ryzen 7 3700X` (`8` cores / `16` threads)
- Memory: `31.91 GB`
- Compiler: `clang++ -std=c++14 -O3` (local run log)

Commands (Linux equivalent):

```bash
# flow perf (exception / strict no-exception)
clang++ -std=c++14 -O3 -I. test/flow_perf.cpp -o /tmp/flow_perf_exc -pthread
clang++ -std=c++14 -O3 -fno-exceptions -DFLUX_FOUNDRY_NO_EXCEPTION_STRICT=1 -fno-rtti -march=native -fstrict-aliasing -I. test/flow_perf_noexcept.cpp -o /tmp/flow_perf_noexc -pthread
/tmp/flow_perf_exc
/tmp/flow_perf_noexc

# flux vs asio horizontal compare
bash bench/horizontal_compare/run.sh
bash bench/horizontal_compare/run_noexcept.sh
```

### Flow perf matrix (Linux, `ns/op`, median of 7 rounds)

| Case | Exceptions ON | Exceptions OFF |
|---|---:|---:|
| `direct.loop20` | 5.86 | 5.84 |
| `runner.sync.20nodes` | 26.84 | 23.60 |
| `fast_runner.sync.20nodes` | 5.86 | 5.53 |
| `runner.awaitable.async4` | 375.30 | 324.19 |
| `fast_runner.awaitable.async4` | 124.24 | 109.73 |
| `runner.fast_awaitable.async4` | 221.58 | 169.25 |
| `fast_runner.fast_awaitable.async4` | 93.58 | 81.56 |
| `runner.awaitable.async1` | 94.45 | 82.11 |
| `fast_runner.awaitable.async1` | 30.32 | 28.26 |
| `runner.fast_awaitable.async1` | 55.75 | 43.97 |
| `fast_runner.fast_awaitable.async1` | 18.93 | 17.39 |
| `runner.when_all.2` | 192.97 | 188.08 |
| `fast_runner.when_all_fast.2` | 125.14 | 126.24 |
| `runner.when_all_fast.2` | 156.34 | 154.37 |
| `runner.when_any.2` | 165.42 | 161.51 |
| `fast_runner.when_any_fast.2` | 95.13 | 95.68 |
| `runner.when_any_fast.2` | 129.05 | 125.68 |

### Horizontal compare (Linux, exceptions ON, core matrix, `ns/op`)

| Case | ns/op |
|---|---:|
| `baseline.direct.loop20` | 6.27 |
| `sched.flux.via20.dispatch` | 246.23 |
| `sched.asio.dispatch20` | 159.26 |
| `sched.flux.via20.post` | 247.76 |
| `sched.asio.post20` | 689.79 |
| `full.flux.runner.async4.post` | 703.32 |
| `full.flux.fast_runner.async4.post` | 481.98 |
| `full.flux.runner.fast_awaitable.async4.post` | 639.63 |
| `full.flux.fast_runner.fast_awaitable.async4.post` | 461.71 |
| `full.asio.post.async4` | 187.36 |
| `full.flux.runner.when_all2.post` | 317.02 |
| `full.flux.fast_runner.when_all2.post` | 258.03 |
| `full.flux.fast_runner.when_all2.fastagg.post` | 262.37 |
| `full.asio.post.when_all2` | 141.28 |
| `full.flux.runner.when_any2.post` | 280.62 |
| `full.flux.fast_runner.when_any2.post` | 237.41 |
| `full.flux.fast_runner.when_any2.fastagg.post` | 215.74 |
| `full.asio.post.when_any2` | 126.85 |

### Horizontal compare (Linux, strict no-exception, expanded matrix excerpt, `ns/op`)

| Case | ns/op |
|---|---:|
| `baseline.direct.loop20` | 6.61 |
| `sched.flux.native.via20.inline` | 101.77 |
| `sched.flux.native.via20.dispatch` | 1459.86 |
| `sched.flux.on_asio.via20.dispatch` | 267.01 |
| `sched.asio.raw.dispatch20` | 166.47 |
| `sched.flux.on_asio.via20.post` | 275.68 |
| `sched.asio.raw.post20` | 812.18 |
| `full.flux.native.fast_runner.fast_awaitable.async4.inline` | 86.30 |
| `full.flux.native.fast_runner.fast_awaitable.async1.inline` | 22.02 |
| `full.flux.on_asio.fast_runner.fast_awaitable.async4.dispatch` | 327.29 |
| `full.asio.raw.dispatch.async4` | 105.31 |
| `full.asio.adapter.dispatch.async4` | 181.02 |
| `full.flux.on_asio.fast_runner.fast_awaitable.async4.post` | 459.56 |
| `full.asio.raw.post.async4` | 203.90 |
| `full.asio.adapter.post.async4` | 284.25 |
| `full.flux.on_asio.fast_runner.fast_awaitable.async1.dispatch` | 150.47 |
| `full.asio.raw.dispatch.async1` | 97.04 |
| `full.asio.adapter.dispatch.async1` | 123.47 |
| `full.flux.on_asio.fast_runner.fast_awaitable.async1.post` | 153.20 |
| `full.asio.raw.post.async1` | 97.38 |
| `full.asio.adapter.post.async1` | 123.45 |
| `full.flux.on_asio.fast_runner.when_all2.fastagg.post` | 265.95 |
| `full.asio.raw.post.when_all2` | 144.90 |
| `full.flux.on_asio.fast_runner.when_any2.fastagg.post` | 212.32 |
| `full.asio.raw.post.when_any2` | 145.91 |

### Real backend async overhead (Linux, strict no-exception, same host/toolchain, `ns/op`)

| Case | ns/op |
|---|---:|
| `real.flux.fast_runner.fast_awaitable.async4.backend` | 5012.02 |
| `real.asio.raw.async4.backend` | 4156.70 |
| `real.flux.fast_runner.fast_awaitable.async1.backend` | 4745.71 |
| `real.asio.raw.async1.backend` | 4255.38 |

Notes:

- Linux snapshot above is sourced from `test/ubuntu_bench.txt` (`flow_perf*`) and `bench/horizontal_compare/linux.txt` (`run*.sh`).
- `run_noexcept.sh` currently covers the full expanded matrix (`57` benchmark cases).
- `run.sh` keeps a compact exception-enabled matrix for quick comparison.
- The current tables are product-realistic: they include framework optimizations such as DSL fusion (`via|via` deduplication).
- If you need strict symmetry against raw scheduler APIs, run a separate benchmark profile with those fusion optimizations intentionally disabled.
- This is a microbenchmark for framework overhead, not an end-to-end workload benchmark.
