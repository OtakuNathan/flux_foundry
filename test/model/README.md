# Formal Models (TLA+)

This directory contains small TLA+ models for concurrency protocols exercised by the runtime tests:

- `flow_controller` cancel-state protocol (`FlowControllerCancel.tla`)
- `simple_executor` ticket accounting / shutdown-drain protocol (`SimpleExecutorTickets.tla`)
- libuv sender-ticket / native-handle close protocol (`UvExecutorClose.tla`)
- io_uring admission / terminal-CQE / detach protocol (`IoUringClose.tla`)
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
- shutdown may be requested before `run()` starts; `run()` may still enter once in shutdown state to drain pre-admitted work
- consumer-thread inline fallback abstracts any enqueue miss (`q.try_emplace()` full/contended miss), not only hard-full queue

### 3) `awaitable_base` / `fast_awaitable_base`
Source reference:
- `flow/flow_awaitable.h`

Modeled properties:
- at-most-once callback delivery (`next_step` invocation)
- normal awaitable cancel path is single-winner vs resume path
- fast awaitable path never registers a controller cancel handler

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
- `when_any` launch-success means "at least one child launched"; remaining children may stop due to winner or partial launch failure

### 6) `utility` queues
Source reference:
- `utility/concurrent_queues.h`

Modeled properties:
- `spsc_queue` ready-bit occupancy/accounting and `mpsc_queue` generation-tag occupancy/accounting
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

### 8) libuv and io_uring attachments

Source references:
- `utility/close_gate.h`
- `executor/uv_executor.h`
- `extension/io_uring_awaitable.h`

The existing `SimpleExecutorTickets` model remains useful for ticket accounting,
but combines admission and enqueue into one transition. It does not cover an
unpublished reservation or a native notification that has woken the consumer
before returning to its sender. The two attachment models add those boundaries
without changing the existing model.

`UvExecutorClose.tla` separates admission, capacity reservation, FIFO slot claim,
publication, native send, send return and ticket release. It also models the
closer's notification ticket, coalesced wakeups, bounded callback draining,
owner rearming, native close initiation and the close callback. Safety checks
cover ticket/queue accounting, at-most-once execution, no send on a closing or
closed handle, and no native close with outstanding senders or queued work.
Publication holes may stop a callback while later slots are already ready.

`IoUringClose.tla` separates admission, slot reservation, SQE preparation,
staging, submission, kernel completion and terminal CQE dispatch. Close and
logical cancellation can interleave with the serialized SQ/CQ owner. Rejected
submissions return their tickets; accepted operations retain tickets and backend
buffers through logical cancellation until their terminal CQE. Partial batches
are represented by individual submissions; failed flushes are stuttering steps.
Safety checks cover bounded slots, ticket/buffer ownership, serialized preparation
and no detach with unresolved I/O.

Both models check `ShutdownCompletes`: once closing is requested, native close
or detach eventually completes. This assumes weak fairness for admitted work,
the closer, the host loop and (for io_uring) kernel completion. It does **not**
promise shutdown if a producer stops forever, the host stops polling/flushing,
or an I/O operation never completes. Close itself is optional, so this is a
conditional progress property. These finite models do not use a step cutoff or
symmetry reduction for their liveness checks. Deadlock checking is disabled
because completed shutdown is an intentional terminal state; liveness is checked
separately and catches a stranded close.

The default configs use three producer/request IDs and capacity two; the `Small`
configs use two IDs and capacity one. The libuv callback budget is one. Each ID
attempts one request. Repeated public close calls and rejected/stale CQEs have
no additional protocol effect and are abstracted as stuttering.

Checked on 2026-11-13 with the existing `../Pal/tla2tools.jar`, TLC 2.19
(2024-08-08, revision `5a47802`), Java 21, one TLC worker, 512 MiB maximum heap:

| Model/config | Distinct states | Result |
| --- | ---: | --- |
| `SimpleExecutorTickets.cfg` | 520 | Existing safety invariants passed |
| `UvExecutorCloseSmall.cfg` | 3,848 | Safety and conditional liveness passed |
| `UvExecutorClose.cfg` | 84,025 | Safety and conditional liveness passed |
| `IoUringCloseSmall.cfg` | 323 | Safety and conditional liveness passed |
| `IoUringClose.cfg` | 4,827 | Safety and conditional liveness passed |

Two negative controls were checked on temporary copies with the small configs:

- Replace `wake' = (wake \/ Ready \/ closing)` with `wake' = (wake \/ Ready)`:
  TLC reports a temporal-property violation (exit 13). The close callback can
  consume the notification while a sender still holds its ticket, then sleep
  forever after the sender returns and releases it.
- Remove `tickets = 0` from the io_uring `Detach` guard: TLC violates
  `NoPrematureDetach` (exit 12), with an accepted request still unresolved.

The original models remain intact. Run logs and the negative-control copies
from this check are under the ignored `build/tla-close/` directory.

Limits: these models assume the queue's release/acquire publication and native
resource lifetime contracts. They do not model C++ weak memory, payload bytes,
physical slot reuse/generation wrap, inline recursive tasks, shared-pointer
reclamation or libuv/kernel internals. In particular, they do not prove the
tail-CAS memory-order fix; the C++ interleaving tests and TSan complement them.
Passing finite instances is not an unbounded proof of the implementation.

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

To reproduce the attachment checks with the jar in the neighboring Pal checkout:

```bash
mkdir -p build/tla-close
for variant in '' Small; do
  java -XX:+UseParallelGC -Xmx512m -cp ../Pal/tla2tools.jar tlc2.TLC \
    -workers 1 -metadir "build/tla-close/uv-${variant}-states" \
    -config "test/model/UvExecutorClose${variant}.cfg" test/model/UvExecutorClose.tla
  java -XX:+UseParallelGC -Xmx512m -cp ../Pal/tla2tools.jar tlc2.TLC \
    -workers 1 -metadir "build/tla-close/uring-${variant}-states" \
    -config "test/model/IoUringClose${variant}.cfg" test/model/IoUringClose.tla
done
```

## Notes / Limits

- The `simple_executor` model assumes the underlying `mpsc_queue` satisfies its MPSC correctness contract.
- The `flow_controller` model abstracts callback function pointers into a boolean `handlerInstalled`.
- The `flow_runner` model focuses on the async-node handshake path; it does not model the full pipeline interpreter.
- The `async_aggregator` model focuses on successful-launch completion races (the most concurrency-sensitive part).
- Queue/static-stack models are protocol abstractions; `StaticListDualStack.tla` is closer to code structure (tagged heads + `next` tags) but still abstracts CAS retries/backoff and payload lifetime.
- The older models are safety-oriented. The attachment models additionally check
  conditional liveness under the stated fairness assumptions; none proves throughput.
