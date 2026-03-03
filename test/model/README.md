# Formal Models (TLA+)

This directory contains small TLA+ models for the two concurrency-heavy pieces that were stress-tested:

- `flow_controller` cancel-state protocol (`FlowControllerCancel.tla`)
- `simple_executor` ticket accounting / shutdown-drain protocol (`SimpleExecutorTickets.tla`)
- `awaitable_base` / `fast_awaitable_base` callback lifecycle (`AwaitableLifecycle.tla`)
- `flow_runner` async-node handshake (factory/lock/set-next/submit/cancel/resume) (`FlowRunnerAsyncNode.tla`)
- `flow_async_aggregator` 2-way `when_all` / `when_any` normal+fast completion protocol (`AsyncAggregator2Way.tla`)
- `utility` queue protocols (`ReadyBitRingQueue.tla`, `MpmcQueueSeq.tla`, `SpmcDequeState.tla`)
- `utility::static_stack` dual-list ownership / sequence-tag discipline (`StaticListDualStack.tla`)

These are *abstract* models, not line-by-line translations of the C++ implementation. They are meant to validate key safety properties and make reasoning explicit.

## What is modeled

### 1) `flow_controller::cancel()` protocol
Source reference:
- `flow/flow_runner.h` (`flow_controller`, lock/cancel state machine)

Modeled properties:
- cancel winner is unique (at-most-once cancel side effect)
- handler fields are cleared after a successful cancel
- lock ownership is consistent with `locked` state
- canceled state is consistent with winner kind (`soft` / `hard`)

### 2) `simple_executor`
Source reference:
- `executor/simple_executor.h`

Modeled properties:
- ticket accounting is preserved (`issued = pending + retired`)
- retire accounting splits into `popped + inlined`
- queue length remains bounded and accounted
- shutdown-phase post-dispatch abort does not leak tickets

### 3) `awaitable_base` / `fast_awaitable_base`
Source reference:
- `flow/flow_awaitable.h`

Modeled properties:
- at-most-once callback delivery (`next_step` invocation)
- normal awaitable cancel path is single-winner vs resume path
- fast awaitable cancel-handler path is a no-op (as designed)

### 4) `flow_runner` async node path
Source reference:
- `flow/flow_runner.h` (async node dispatch path)

Modeled properties:
- receiver callback is at most once across submit-fail / cancel / resume outcomes
- cancel-handler drop notification is at most once
- lock token ownership is consistent with controller locked state
- terminal paths do not leak handler registration/lock ownership

### 5) `flow_async_aggregator`
Source reference:
- `flow/flow_async_aggregator.h`

Modeled properties (2-way abstract model):
- `when_all` / `when_any` result selection rules
- at-most-once aggregator `resume()`
- `winner` / `failedIdx` consistency
- normal vs fast cancellation side effects (`controllersCanceled`)

### 6) `utility` queues
Source reference:
- `utility/concurrent_queues.h`

Modeled properties:
- `spsc_queue` / `mpsc_queue` ready-bit ring occupancy and accounting
- `mpmc_queue` slot state/round discipline (empty/full claim/publish protocol)
- `spmc_deque` owner/thief slot-state protocol (`private/shared/claimed/empty`)

### 7) `utility::static_stack`
Source reference:
- `utility/static_stack.h`

Modeled properties:
- explicit tagged heads (`head_` / `free_`) and per-node `next` tags (`nodes[i].next`)
- node ownership partition (`head` / `free` / transient reservations)
- capacity conservation
- push/pop publication accounting (`pop_from_list` + `append_to_list` successful-step abstraction)
- per-node sequence-tag discipline and list-link tag consistency (ABA-mitigation abstraction)

## How to run TLC (when `tla2tools.jar` is available)

From repo root:

```bash
java -cp /path/to/tla2tools.jar tlc2.TLC test/model/FlowControllerCancel.tla -config test/model/FlowControllerCancel.cfg
java -cp /path/to/tla2tools.jar tlc2.TLC test/model/SimpleExecutorTickets.tla -config test/model/SimpleExecutorTickets.cfg
java -cp /path/to/tla2tools.jar tlc2.TLC test/model/AwaitableLifecycle.tla -config test/model/AwaitableLifecycleNormal.cfg
java -cp /path/to/tla2tools.jar tlc2.TLC test/model/AwaitableLifecycle.tla -config test/model/AwaitableLifecycleFast.cfg
java -cp /path/to/tla2tools.jar tlc2.TLC test/model/FlowRunnerAsyncNode.tla -config test/model/FlowRunnerAsyncNodeNormal.cfg
java -cp /path/to/tla2tools.jar tlc2.TLC test/model/FlowRunnerAsyncNode.tla -config test/model/FlowRunnerAsyncNodeFast.cfg
java -cp /path/to/tla2tools.jar tlc2.TLC test/model/AsyncAggregator2Way.tla -config test/model/AsyncAggregator2WayWhenAll.cfg
java -cp /path/to/tla2tools.jar tlc2.TLC test/model/AsyncAggregator2Way.tla -config test/model/AsyncAggregator2WayWhenAllFast.cfg
java -cp /path/to/tla2tools.jar tlc2.TLC test/model/AsyncAggregator2Way.tla -config test/model/AsyncAggregator2WayWhenAny.cfg
java -cp /path/to/tla2tools.jar tlc2.TLC test/model/AsyncAggregator2Way.tla -config test/model/AsyncAggregator2WayWhenAnyFast.cfg
java -cp /path/to/tla2tools.jar tlc2.TLC test/model/ReadyBitRingQueue.tla -config test/model/ReadyBitRingQueueSPSC.cfg
java -cp /path/to/tla2tools.jar tlc2.TLC test/model/ReadyBitRingQueue.tla -config test/model/ReadyBitRingQueueMPSC.cfg
java -cp /path/to/tla2tools.jar tlc2.TLC test/model/MpmcQueueSeq.tla -config test/model/MpmcQueueSeq.cfg
java -cp /path/to/tla2tools.jar tlc2.TLC test/model/SpmcDequeState.tla -config test/model/SpmcDequeState.cfg
java -cp /path/to/tla2tools.jar tlc2.TLC test/model/StaticListDualStack.tla -config test/model/StaticListDualStack.cfg
```

If you use the TLA+ Toolbox, open the `.tla` file and use the provided `.cfg` contents for the model.

## Notes / Limits

- The `simple_executor` model assumes the underlying `mpsc_queue` satisfies its MPSC correctness contract.
- The `flow_controller` model abstracts callback function pointers into a boolean `handlerInstalled`.
- The `flow_runner` model focuses on the async-node handshake path; it does not model the full pipeline interpreter.
- The `async_aggregator` model focuses on successful-launch completion races (the most concurrency-sensitive part).
- Queue/static-stack models are protocol abstractions; `StaticListDualStack.tla` is closer to code structure (tagged heads + `next` tags) but still abstracts CAS retries/backoff and payload lifetime.
- These are safety-oriented models; they do not attempt to fully prove all liveness/throughput properties.
