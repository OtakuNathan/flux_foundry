# Pi 4B perf findings — 2026-09-30

## Collection / evidence

Profiled six configurations: libuv64 raw/fast/normal, libuv10 fast, existing gsource fast, existing simple fast. Same overload2m workload, capacity 4096, 262144 events × 3 repetitions = **786432 events per configuration**, work128, caller CPU3 / consumer CPU2. Built the existing benchmark with `-O3 -g -fno-omit-frame-pointer -march=native`, exceptions ON, Clang 19.1.7. The libuv self-test passed on this profiling build. No executor or workload semantics were changed.

`perf stat` and `perf record` were separate executions. `perf record`: `cycles:u`, requested 299Hz, frame-pointer callchains. Recorded 767–1289 samples/configuration. Percentages below are weighted by sample periods, not unweighted counts, and producer/consumer denominators are separate. Inlining and outlined atomic helpers limit exact attribution. Sampling is modest; treat percentages as approximate hotspot indicators, not precise accounting.

Environment/source/binary digests: `env.txt`. Counter totals: `*-stat.txt` and `counters.csv`. Condensed per-thread symbols/caller evidence: `summary.txt`, full symbol strings in `summary.json`. All sampled-run CSVs have three validated cases. Raw `.data`, full perf reports and perf-script files remain in `/tmp/ff_perf_20260930` (not committed/preserved in this results directory).

This host allows user-space PMU events, but not scheduler tracepoints. `sched:sched_switch` probe failed: no permission to read `/sys/kernel/tracing/events/sched/sched_switch`. No kernel permissions/settings were changed. `context-switches:u`/`cpu-migrations:u` produced zeros; these are **not evidence of no scheduling or context switches**. No off-CPU or kernel-cycle analysis was obtained, so the previous idle millisecond tails remain unexplained.

This perf version rejects `--sort tid` while returning exit 0; the first reports were empty. Reports were regenerated using supported `--sort pid,dso,symbol` and checked nonempty. Perf-script PID/TID fields explicitly distinguish producer from consumer. Percentages retain unknown samples in their denominators.

## 1. Normal adds ownership/state and allocator work

Libuv64 consumer sampled self time:

| Category | Fast | Normal |
|---|---:|---:|
| Outlined AArch64 atomic helpers | 10.94% | **27.67%** |
| Explicit libc allocation/free functions | 0.30% | **4.11%** |
| Synthetic `load_probe::work` function | 54.33% | 40.90% |

Producer sampled self time:

| Category | Fast | Normal |
|---|---:|---:|
| Outlined atomic helpers | 8.22% | **17.16%** |
| Explicit libc allocation/free functions | 0.33% | **6.31%** |

Atomic-helper caller stacks in normal point to async-node dispatch, `awaitable_base::submit_async`, `flow_runner::ipc::make_async_next_step`, `awaitable_base::do_resume`, and libc aligned allocation/free internals. About 10.69% of consumer sampled weight is in atomic helpers immediately called from the normal continuation, 4.20% from normal do_resume, 7.59% from `_int_free_chunk`. These are disjoint **helper samples**, not all inclusive cost of each component.

Source confirms this workload creates a fresh normal runner/event. `flow_runner.h:355–370` initializes a controller; current `FLUX_FOUNDRY_FLOW_CONTROLLER_ALIGNED_ALLOC=1` calls `make_lite_ptr_with_allocator(...aligned_malloc_allocator{})`, whose allocation reaches posix_memalign. `flow_runner.h:508–528` resumes through controller state CAS/fetch_add, cancel-handler reset and continuation ownership. Thus allocator/state costs have source + stack evidence; this is no longer only a speculative explanation. It does **not** mean every possible use of the normal runner has the same per-event allocation topology.

The benchmark itself has an atomic completion counter even in raw mode: raw consumer atomic helpers occupy 6.24%. Some fast helper weight is likewise the instrumented completion path; do not attribute every sampled atomic operation to framework reference counts. libc atomic helpers also represent allocator machinery, not exclusively ff state.

## 2. Generic cache-miss counts rise with normal, but cause is not fully isolated

Whole-command counters divided by equal event counts:

| Libuv64 lane | User cycles/event | Instructions/event | Generic cache misses/event | IPC |
|---|---:|---:|---:|---:|
| raw | 5746 | 4852 | 9.51 | 0.844 |
| fast | 6893 | 5207 | 25.88 | 0.755 |
| normal | 9299 | 6690 | **66.45** | 0.719 |

Normal has about 35% more counted user cycles and **2.57× generic cache misses** than fast in these runs. These totals include initialization, finite-run generation, sample validation and five percentile-array sorts, not just the dispatch/compute path. They are not NIC-to-order cycle costs. `cache-misses` is the PMU generic event; it is not established here as LLC misses or as proof of false sharing. The profile supports investigating controller allocation, ownership transfers and locality, but no cache-coherence/c2c or PMU stall breakdown was collected.

## 3. Cross-thread freeing limits TLS-cache usefulness

Even fast samples show explicit global pool allocation/deallocation and atomic-helper callers from `static_mem_pool`. Source `memory/pooling.h:62–96` uses a thread-local cache: allocations consume the allocating thread's cache, frees populate the freeing thread's cache, then spill to a shared static pool / libc. In this benchmark, the producer creates awaitables and the consumer completes/releases them. The source itself notes cross-thread frees may reduce locality (`pooling.h:158–159`).

This is a concrete optimization lead, not proof that it is the largest bottleneck: consider bounded owner-return recycling or aligned controller pools, with lifetime/drain rules preserved. Do not weaken atomics without a protocol proof; do not invoke a consumed async runner again; do not return controller/awaitable storage to a pool while callbacks can still reference it.

## 4. Gsource notification syscall load is substantial

Whole-command system CPU time from the non-sampling perf-stat runs:

- gsource fast: **0.890s**.
- libuv10 fast: **0.231s**.
- libuv64 fast: **0.096s**.

Kernel stacks were not sampled, so these numbers alone do not identify a syscall. Source plus a separate small syscall census corroborate the notification path. `gsource_executor.h:170` schedules an eventfd write after each admitted task; its callback reads eventfd and handles up to ten tasks. The uv adapter uses uv_async coalescing and budget rearm.

Supplementary `strace -f -c`, 8192 events, one run (timing is strongly perturbed by tracing):

| Observed syscall count | Gsource | Libuv64 |
|---|---:|---:|
| write | **8200** | 134 |
| read | 4052 | 138 |
| ppoll / epoll_pwait | 4044 ppoll | 130 epoll_pwait |

Counts include setup, CSV output/fence/stop traffic and any selected-syscall library work. They are **not** direct throughput results and cannot be extrapolated as exact counts for untraced execution; coalescing is timing-sensitive. Still, near-one-write/event in gsource has both source and census evidence, while uv64 batches/coalesces notifications in this particular run.

A gsource optimization candidate is safely coalesced wake notification plus a configurable per-round budget. That requires lost-wakeup and IO/timer-fairness tests; simply suppressing writes without a correct armed/disarm/recheck protocol is unsafe. No production gsource change was made.

## 5. Backpressure and instrumentation remain visible

Producer user samples spend substantial weight in dispatch: uv64 fast **26.81%**, uv10 fast 30.32%, simple fast 25.20%, gsource fast 12.72%. This is an overload workload with full-queue spinning, not a measurement of the uncontended dispatch instruction count. Kernel portions of gsource notification are absent from those percentages, so ranking backends by this column alone is invalid.

Producer sample sorting consumes about 9–11%; timestamp helpers about 7–11%. Consumer timestamp helper weight also remains visible. `load_probe::work` includes callback timestamping and synthetic arithmetic, not only real business computation. This explains why whole-process profiling cannot be presented as an exact live trading-engine cost breakdown.

The simple executor is not permanently pure pause-spin: `back_off.h:24–38` escalates from architecture yield hints to `std::this_thread::yield()`. Whole-program runs include long consumer-idle periods during producer-side result sorting. Its 0.832s system CPU cannot be assumed to be useful task execution. No syscall census or kernel-stack attribution for simple was collected in this round.

## Optimization priority suggested by evidence

1. Keep the latency-critical, stable-owner compute path synchronous/fast; use normal when its cancellation/lifecycle semantics are required.
2. Investigate aligned controller pooling and owner-aware return/recycling before attempting memory-order changes. Validate bounded memory, cross-thread lifecycle and shutdown.
3. For the existing gsource adapter, experiment with safe notification coalescing and configurable batch budget; verify no lost wakeups and no timer/IO starvation.
4. Diagnose idle long tails separately with authorized scheduler/off-CPU tracing and a better-isolated runtime environment. Current permissions are a blocker for that evidence, not for the completed user-space profiling.

No implementation optimization, dependency install, sysctl change or service restart was performed. No performance guarantee is inferred from profiled timings.

## Reproduction

```bash
CXX=clang++ FF_EVENT_BINARY=/tmp/ff_event_profile \
EXTRA_CXXFLAGS='-march=native -g -fno-omit-frame-pointer -Wall -Wextra -Werror' \
bash bench/event_loop/run.sh --self-test

perf stat -e cycles,instructions,cache-misses,branch-misses -- \
  /tmp/ff_event_profile --backend libuv --batch 64 --mode normal \
  --profile overload2m --samples 262144 --repeats 3 \
  --producer-cpu 3 --consumer-cpu 2

perf record -o /tmp/normal.data -e cycles:u -F 299 -g --call-graph fp -- \
  /tmp/ff_event_profile --backend libuv --batch 64 --mode normal \
  --profile overload2m --samples 262144 --repeats 3 \
  --producer-cpu 3 --consumer-cpu 2
perf report -i /tmp/normal.data --stdio --no-children -g none --sort pid,dso,symbol
perf script -i /tmp/normal.data -F comm,pid,tid,time,event,period,ip,sym,dso \
  > /tmp/normal-script.txt
python3 bench/event_loop/perf_summary.py /tmp --json-output /tmp/perf-summary.json
```

The summary tool collects all `*-script.txt` files in the supplied directory; use a dedicated directory to avoid mixing unrelated captures. Percentages are per-thread, self-symbol user-space sample periods, not off-CPU time or a complete wall-time partition.
