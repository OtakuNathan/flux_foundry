# Scheduled-arrival queue capacity probe

Compare `simple_executor<256/1024/4096/16384>` with raw dispatch, normal runner + normal awaitable, and fast runner + fast awaitable. This is an instrumented single-producer/single-consumer load probe, not a production trading benchmark or a claim about every ff executor.

```bash
# Linux/Pi example; choose two CPUs allowed by your affinity mask.
CXX=clang++ EXTRA_CXXFLAGS='-march=native' \
  bash bench/queue_capacity/run.sh --producer-cpu 3 --consumer-cpu 2 \
  > /tmp/ff_queue_capacity.csv 2> /tmp/ff_queue_capacity.log

# Small correctness run; no affinity required.
bash bench/queue_capacity/run.sh --samples 1024 --repeats 1

# Reuse the compiled binary without recompiling.
/tmp/ff_queue_capacity --capacity 4096 --mode fast --profile burst8192 \
  --samples 32768 --repeats 3 --producer-cpu 3 --consumer-cpu 2
```

Defaults: 32768 events/case, 3 repetitions, 128 iterations of identical unsigned dependency-chain work/event. `--work` changes work, `--samples` and `--repeats` bound runtime. Each operation has **one** async queue dispatch; this is not the earlier four-hop microbenchmark. Blueprints and sample arrays are constructed before timed arrival generation; a fresh runner is created per flow event. Raw intentionally omits flow ownership/cancellation semantics, so it is a lower-bound reference, not a feature-equivalent competitor. No cancellation is exercised.

## Arrival profiles

- `steady100k`: fixed 100k scheduled events/s.
- `steady500k`: fixed 500k scheduled events/s.
- `overload2m`: fixed 2M scheduled events/s (name is a target stress level, not proof every host is overloaded).
- `burst8192`: 8192 events share a scheduled timestamp, every 20ms (409600/s average for complete bursts).
- `stall5ms`: fixed 500k/s, with a 5ms consumer sleep at event `samples/4`. Actual sleep duration may exceed 5ms due to scheduling.

The producer does **not** wait for completion between submissions. A full queue can still block inside `dispatch`: the executor's existing spin/consumer-inline semantics are unchanged. Planned timestamps are computed from the original schedule and never reset when producer submission falls behind. Every scheduled event is eventually attempted and completed; delayed production is counted as arrival-to-submit lag instead of silently dropping missed arrivals. This models a finite upstream backlog; it is not a physically independent NIC arrival source.

## CSV units and metrics

All `_us` fields are microseconds, `_ms` fields milliseconds.

- `e2e_*`: receiver completion minus **planned arrival**. Includes upstream submit lag, runner setup, queue wait, work and flow completion; avoids measuring only successfully admitted events.
- `actual_p99_us`: receiver completion minus actual submission start. Can look good even when the producer has fallen behind: compare `submit_lag_*`.
- `dispatch_*`: time spent inside the common wrapper around executor dispatch. Includes ordinary admission overhead, clock overhead and full-queue waiting; **not** a separately instrumented exact full-queue stall counter.
- `service_p50_us`: receiver completion minus work callback entry; includes payload work, optional stall and flow completion, but not all cleanup after the receiver.
- `completed_per_s`: sample count divided by elapsed time from first planned arrival to last receiver completion. Finite burst start/end effects mean this is not the infinite-stream average rate.
- `peak_outstanding`: maximum observed submitted/currently-attempted minus completed operations; **not exact queue occupancy**. Includes the producer's current operation and any running callback.
- `late_events`: submission lag greater than one nominal interval (steady profiles) or 1ms (bursts).
- `drain_last_submit_ms`: last completion minus last submission return, clamped at zero.
- `drain_schedule_ms`: last completion minus last planned arrival. Unlike last-submit drain, it retains the cost of a producer blocked through the end of the schedule.
- `sample_storage_mib`: instrumented sample memory, distinct from executor bytes printed on stderr.

At the end of each case, all receiver completions are awaited, then an ordered executor fence waits past preceding backend cleanup. Shutdown drains admitted tickets and joins the consumer. The probe validates exactly one successful callback/event, timestamp ordering and every work checksum; failure aborts with a nonzero exit. Samples are preallocated/touched, but there is no separate untimed flow-pool warmup, so initial pool setup is included. Instrumentation and sample cache traffic affect absolute latency. CPU affinity does not isolate a CPU, disable interrupts or control frequency. Repeated cases rotate lane order but capacity order remains fixed.

Not covered: multiple producers, cancellation under load, sustained hours-long stability, IO/network traffic, live strategy/OMS work, hard real-time guarantees. Inspect both tail latency and submit lag before claiming that a larger queue improves performance.
