# Separate blueprint, awaitable and runner costs

The initial `fast` lane selected a fast **runner** with the public ordinary
awaitable. Awaitable type belongs to the blueprint; runner selection is a
separate dimension and cannot automatically change that type's contracts.

This follow-up holds the fast runner fixed and independently varies:

- `current`: public ordinary awaitable, owning `lite_ptr` blueprint.
- `view`: public ordinary awaitable, existing `make_fast_runner_view`.
- `terminal`: benchmark-only `fast_awaitable_base` node, owning blueprint.
- `terminal_view`: benchmark-only fast node and borrowed blueprint.

The terminal variants deliberately remove controller-driven logical
cancellation and rely on exactly one terminal backend callback. A failed
submission must never callback. Borrowed blueprints stay alive across every
completion in this benchmark. These are changed contracts, not drop-in
optimizations of the public cancelable adapters. No public implementation was
changed. The experimental types only test successful terminal completion here;
they have not received the public adapters' complete lifecycle test suite.

## Measured results

Same 3700X host and dependencies as the original snapshot. Seven rotated
process trials per variant, 1,048,576 requests/bursts per trial, pinned to CPU 3.
io_uring uses Clang 23; DPDK uses GCC 16.2.1 to match the GCC diagnostic baseline.
Every original completion, checksum, close/detach and mbuf-return check remains.

| Variant | io_uring real NOP, batch 32, ns/request | io_uring userspace-only, ns/request | DPDK 1 packet/burst, ns/burst |
|---|---:|---:|---:|
| current | 154.676 | 75.337 | 70.286 |
| view | 130.228 | 51.758 | 47.124 |
| terminal | 138.289 | 59.874 | 48.638 |
| terminal_view | 112.878 | 34.385 | 28.950 |

The io_uring real-NOP comparison saves about 16 ns/request by explicitly
selecting the fast node, about 24 ns by borrowing the blueprint, and about
42 ns with both. These are differences between full paths; they are not
independently additive component timers. The original raw/context baselines
were about 86/93 ns per real NOP. A native NOP has no useful device work, so
small framework bookkeeping costs are a large percentage of the total.

DPDK batch-32 medians were 348.810 / 317.609 / 344.517 / 311.864 ns/burst in
the same variant order. Ranges overlap substantially (terminal_view reaches
418.931 ns). Those differences are less conclusive than the one-packet lanes.
Generated diagnostic binaries alter layout and their case-selection loop;
compare within this diagnostic matrix rather than subtracting from the older
314 ns/burst snapshot. This remains software ring loopback, not physical NIC I/O.

## Allocation evidence

Separate linker-wrapper runs bracketed the owner-thread measurement window:

| Original lane, 1,048,576 operations | direct malloc calls | aligned allocation calls | global new calls |
|---|---:|---:|---:|
| io_uring raw | 0 | 0 | 0 |
| io_uring context | 0 | 0 | 0 |
| io_uring normal runner | 0 | 1,048,576 | 0 |
| io_uring fast runner | 0 | 0 | 0 |
| DPDK fast runner, batch 1 and 32 | 0 | 0 | 0 |

Thus the original **fast-runner** gap is not explained by a system allocation
on every request in this warmed workload. Awaitable class-specific `new/delete`
still take/release cached objects in `memory/pooling.h`; those operations are
not counted as system allocations. Normal runners additionally initialize a
per-invocation controller in `flow_runner::init_controller`, backed by the
aligned allocator. That matches one observed aligned allocation per request.
The wrapper does not observe allocations internal to shared libraries, nor is
it a general memory profiler. Instrumented timing values in the logs are ignored.

## Source and generated-code explanation

Source inspection of the successful fast-runner/public-awaitable path identifies:

- Blueprint ownership copies through the factory and asynchronous continuation,
  with atomic reference increments/decrements in `memory/lite_ptr.h`. Borrowed
  storage avoids those ownership operations while strengthening host lifetime
  requirements.
- Ordinary awaitable state transitions use CAS at submit and completion.
  A separate backend retain/release pair keeps the object alive if logical
  cancellation consumes the flow reference first. The terminal experiment has
  no such competing completion path, so it omits those two state CAS operations
  and that extra backend reference pair.
- Even `fast_awaitable_base` has reference bookkeeping to support completion
  during submit, plus cached object acquisition/release and a type-erased
  continuation. `io_uring_context` still has its own admission/close tickets and
  generation-token routing. Fast runner + fast node is not equivalent to raw
  liburing preparation.

Clang's disassembly confirms locked reference-count operations in the fast
runner's blueprint capture path and locked state CAS in the public awaitable's
completion path. This is source/assembly inspection plus controlled variants,
not PMU sampling; it does not assign an exact cycle count to each instruction.

## Reproduction and evidence

Use [`diagnose.py`](../../../diagnose.py) and
[`trace_alloc.py`](../../../trace_alloc.py) as described in the
[harness README](../../../README.md). The public `uring.cpp` and `dpdk.cpp`
are unchanged; generated sources and binaries reside under the requested build
directory. `diagnostic_terminal.h` contains the explicitly different node types.

- [io_uring raw trials](uring/results.csv), [compile commands](uring/environment.json),
  [allocation trace](uring/alloc.log).
- [DPDK raw trials](dpdk/results.csv), [compile commands](dpdk/environment.json),
  [allocation trace](dpdk/alloc.log).
