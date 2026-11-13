# Asio / flux_foundry comparison

## Current bounded-concurrency battle

[2026-11-13 results: Ryzen 3700X and M1 Max](results/2026-11-13/README.md)
contain 1,056 independent process trials with matched logical work, real Asio
coroutines, per-case variability, tail latency and CPU measurements.

Run from the repository root with Python 3, a C++20 compiler, git, libuv and
pkg-config installed:

```sh
python3 bench/horizontal_compare/battle.py
# Linux, separate physical cores (choose IDs for your machine):
python3 bench/horizontal_compare/battle.py --owner-cpu 3 --producer-cpu 2
```

The runner creates a new `build/asio-battle-TIMESTAMP/` directory, fetches
standalone Asio **1.39.0** at commit
`190a8d8daf1cc62909b4e0c72ff491ba264ca98e`, and builds every lane in one
C++20 `-O3 -DNDEBUG` binary with exceptions enabled. It does not change system
settings or install packages. `--asio PATH` reuses a clean checkout at that
exact commit. `--out PATH` must name a new directory. `--cxx` selects the compiler.
On macOS, use AC power and leave CPU IDs at -1.

Defaults: six independent processes per case, 131,072 measured requests after
4,096 warmup requests, 128 unsigned arithmetic iterations **per work step**,
1,024 outstanding requests, one producer and one consumer. Both loops use the
same native blocking or polling policy within each comparison. macOS excludes
the experimental Linux-only `--policies hybrid-2000` candidate; the standard
matrix uses `blocking poll` on both platforms. This changes no production
executor defaults.

Two groups have deliberately different contracts:

| Group | Contract | What it compares |
|---|---|---|
| `chain` | One initial consumer envelope, then 1 or 4 explicitly posted asynchronous work steps; all on the same Asio `io_context` | Callback / coroutine / FF flow construction and execution |
| `handoff` | Producer starts a request; one queued work step and completion on the consumer | Asio vs FF's UV executor, with callback and FF flow lanes |

The chain uses Asio for all lanes because UV dispatch may inline when already
on its owner thread. Asio coroutine startup uses `co_spawn`'s native initial
dispatch as its envelope; other lanes post an explicit envelope. Extra native
coroutine bookkeeping is included, not claimed to be an identical internal
instruction/event count. The handoff group excludes coroutines because adding
`co_spawn` would change that one-step contract. UV uses capacity 1,024 and drain
budget 16; Asio's queue is unbounded internally but the same external credit
limit bounds outstanding logical requests on both backends.

| Lane | Implementation |
|---|---|
| `raw` | Typed native callback chain |
| `erased` | Callback work steps through FF `task_wrapper_sbo` |
| `coroutine` | Real `asio::awaitable` / `co_spawn`, fresh coroutine per request |
| `ff-normal` | Ordinary awaitable, normal runner, owning blueprint handle |
| `ff-fast-normal` | Ordinary awaitable, fast runner, owning blueprint handle |
| `ff-fast-fast` | Fast awaitable, fast runner, borrowed immutable blueprint view |

Blueprints are reused, **runners are fresh per request**. A borrowed blueprint
outlives the entire measured batch. The last two lanes differ in both awaitable
and blueprint ownership; their difference is not an isolated awaitable cost.
Pools and Asio's allocator optimizations remain at library defaults after
warmup. Allocation/recycling and completion bookkeeping inside the timed batch
are included; allocation counts are not instrumented. Callback payloads hold
the message directly, while FF awaitables retain it in awaitable state, so the
`erased` lane aligns the callable boundary rather than every object size.

Every request validates its unsigned checksum, exact number/order of work
steps, envelope count, exactly one completion, consumer-thread execution and
timestamp ordering. A consumer fence drains final completion cleanup before
samples and blueprints are destroyed. Compile-time checks cover the clock,
lock-free credit counter, payload movement and supported graph sizes; FF's own
template checks enforce its callable/runner contracts.

All lanes perform the same arithmetic and logical async work, but this is a
**successful completion benchmark**, not cancellation/error-propagation or
`when_all`/`when_any` equivalence. It measures in-memory scheduling and flow
execution, **not network I/O, io_uring or DPDK performance**. Timestamping,
validation counters and credit management add common instrumentation overhead.

### Reading the output

- `saturated`: bounded, closed-loop producer; `completed_per_s` measures achieved
  throughput for this instrumented workload. Its planned time is the attempt
  start, not an independent offered-load schedule.
- `steady500k`: open-loop planned arrivals every 2 us; submission waits on a
  credit if necessary. `e2e` includes all lateness since planned arrival,
  `actual` starts at the submission attempt and includes admission waiting,
  `admitted` starts after a credit becomes available. Separate submit-lag and
  admission-wait percentiles explain overload. Completing 500k/s is not a
  measurement of maximum throughput.
- CPU is process CPU time / wall time, so 200% means two busy cores. The busy
  producer is included. Timing includes a 2 ms lead-in and final fence for CPU;
  throughput runs from the first planned arrival to the last completion.
- The runner rotates/reverses variants within matched case blocks, shuffles
  blocks with a recorded seed, and saves raw warmup/measured CSV, stderr, all
  commands, source hashes, dependency revision, host/compiler/power metadata,
  per-process results and median/min/max summaries. Medians of process p99s
  are not pooled request percentiles. Never add independently computed p99s.

Useful subsets (all filters are optional):

```sh
python3 bench/horizontal_compare/battle.py --groups chain --hops 4 --profiles saturated
python3 bench/horizontal_compare/battle.py --groups handoff --lanes raw ff-fast-normal ff-fast-fast
# Small semantic smoke matrix:
python3 bench/horizontal_compare/battle.py --trials 1 --samples 2048 --work 8
```

## Legacy serial microbenchmarks

The scripts below are retained for historical continuity. Their results are
not interchangeable with the bounded-concurrency battle: some FF paths use a
full flow while Asio uses hand-written callbacks, `via20` and Asio's 20-step
chain do different arithmetic, and serial submit/drain loops do not exercise
the same cross-thread pooling behavior. Differences can favor either side.
They fetch an unpinned Asio HEAD and do not include real Asio C++20 coroutines.
Use `battle.py` for new reproducible measurements.

Microbenchmark entry point for `flux_foundry` vs standalone Asio on the same host and toolchain.
Results are split into two layers:

- `pure scheduling overhead`: isolates scheduling-path cost as much as possible (`via` vs `dispatch/post`)
- `full semantics overhead`: keeps full `result/await/when_all/when_any` semantics in the path

## Run

```bash
bench/horizontal_compare/run.sh
```

On first run, the script auto-fetches Asio into `/tmp/flux_foundry_asio` and does not place third-party code inside this repository.

`noexcept` variant:

```bash
bench/horizontal_compare/run_noexcept.sh
```

## Current Coverage

- `baseline.direct.loop20`

Pure scheduling overhead:

- `sched.flux.via20.dispatch`
- `sched.asio.dispatch20`
- `sched.flux.via20.post`
- `sched.asio.post20`

Full semantics overhead:

- `full.flux.runner.sync20`
- `full.flux.fast_runner.sync20`
- `full.flux.runner.async4.post`
- `full.flux.fast_runner.async4.post`
- `full.asio.post.async4`
- `full.flux.runner.when_all2.post`
- `full.flux.fast_runner.when_all2.post`
- `full.flux.fast_runner.when_all2.fastagg.post`
- `full.asio.post.when_all2`
- `full.flux.runner.when_any2.post`
- `full.flux.fast_runner.when_any2.post`
- `full.flux.fast_runner.when_any2.fastagg.post`
- `full.asio.post.when_any2`

Real backend async overhead (noexcept matrix):

- `real.flux.fast_runner.fast_awaitable.async4.backend`
- `real.asio.raw.async4.backend`
- `real.flux.fast_runner.fast_awaitable.async1.backend`
- `real.asio.raw.async1.backend`

## Notes

This is a "semantics-near" microbenchmark, not a full feature-equivalence evaluation.
For publication-grade comparisons, further align cancel semantics, error propagation semantics, scheduling model, and allocation accounting methodology.
