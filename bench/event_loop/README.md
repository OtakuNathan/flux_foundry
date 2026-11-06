# Real event-loop load probe

Linux/POSIX benchmark of ff flows driven by:

- `simple`: existing busy-poll `simple_executor` baseline.
- `gsource`: existing `gsource_executor`, GLib `GMainLoop`, eventfd notification, fixed 10-task callback budget.
- `libuv`: **benchmark-only** adapter using ff MPSC queue + `uv_async`; default compares 10 and 64 tasks/callback. Notifications can coalesce; budget exhaustion rearms the async handle.

No DPDK, NIC, socket ingestion, actual trading strategy, exchange IO or production OMS is included. Each event does the same 128-iteration unsigned dependency-chain work used in the capacity probe, with one asynchronous executor handoff. This tests the execution substrate, **not** a complete quantitative-trading engine.

## Run

GLib and libuv development files must be visible to `pkg-config`. The current Pi has GLib 2.84.4 and libuv 1.50.0.

```bash
CXX=clang++ EXTRA_CXXFLAGS='-march=native -Wall -Wextra -Werror' \
  bash bench/event_loop/run.sh --samples 32768 --repeats 3 \
  --producer-cpu 3 --consumer-cpu 2 > /tmp/event-loop.csv

# Existing binary, selected backend and budget.
/tmp/ff_event_loop --backend libuv --batch 64 --capacity 4096 \
  --mode fast --profile steady500k --samples 32768 --repeats 3 \
  --producer-cpu 3 --consumer-cpu 2

# Tiny queues exercise nested owner submission, coalesced wakeups and teardown.
/tmp/ff_event_loop --self-test

python3 bench/event_loop/summarize.py /tmp/event-loop.csv
```

`--help` lists all options. Capacity defaults to 4096; supported values are 256, 1024, 4096, 16384. `--backend all` runs simple, gsource, libuv budget 10, libuv budget 64 in that order. `--batch N` explicitly selects a single libuv budget. Lane order rotates across repetitions, but backend/capacity order is fixed: do not mistake shared-host disturbances for capacity effects.

**All backends are compiled with exceptions enabled**, because existing gsource setup has a throwing error path. The load payload uses `std::exception_ptr` in this build. Compare against the simple baseline in the **same CSV**, not only the older no-exception capacity snapshot. The older capacity executable/source/results are retained unchanged; `bench/load_probe.h` derives the same schedule/checksum/measurement logic and adds optional consumer CPU measurement.

## Metrics / arrivals

Uses planned-arrival timestamps, finite no-drop schedules and per-event checksum/exactly-once checks, just like [the capacity probe](../queue_capacity/README.md). Profiles: idle1k, steady100k, steady500k, overload2m, burst8192, stall5ms. `idle1k` has 1000 scheduled arrivals/s and is capped at 1024 events/run; other profiles use `--samples`. All work is synthetic and identical across backends.

CSV prepends `backend,budget` to the capacity-probe metrics and appends:

- `consumer_cpu_ms`: Linux per-thread CPU clock delta, around event generation through the completion fence, excluding sorting/report generation.
- `consumer_cpu_pct`: consumer CPU / elapsed wall time in that window; includes the common 2ms initial scheduling lead-in and final fence. This is percent of **one consumer core**, not process-wide CPU or an energy measurement.

The producer **busy-waits** on the arrival schedule in every backend. Event loops sleep in poll when idle, but the benchmark process as a whole is not idle. There is no sleep tuning, CPU isolation, real-time scheduling, timer/IO-fairness measurement or profiling. Long idle tails must not be attributed to a specific cause without further evidence.

`budget` only describes maximum tasks per callback. It does not turn 64 small tasks into one payload, and it is not the queue capacity. A larger callback budget can improve burst amortization, but may delay other IO/timers; that fairness tradeoff is **not measured here**.

## Lifecycle / contracts

Contexts/handles are initialized before producer use. Each case waits for receiver completions and sends an ordered queue fence so preceding backend cleanup has finished before sample/context destruction. Producer submission ends before backend stop. GLib quits on its owner; libuv closes its async handle on its owner and checks `uv_loop_close`. Final-send/close synchronization prevents the async handle from closing while the last producer send is still executing. Queues use the existing full-queue producer spin / owner-inline pattern, and all backend objects use aligned allocation for C++14.

`--self-test` validates uv capacities 1/2 with budgets 1/10/64, nested full-queue owner submission, 1000 external events, budget rearm and clean closure. Gsource's existing contract test is separate (`test/gsource_executor_contract.cpp`). No cancellation-under-load or multiple-producer test is included.

[Pi measurement notes](results/pi4b-20260930-notes.md) and [full medians/ranges](results/pi4b-20260930-summary.md).
