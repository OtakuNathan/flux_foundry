# Runtime validation

Build and run the smoke and stress suites with assertions enabled:

```sh
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_STANDARD=14 \
  -DFLUX_FOUNDRY_BUILD_TESTS=ON -DFLUX_FOUNDRY_WARNINGS_AS_ERRORS=ON
cmake --build build-debug --parallel 2
ctest --test-dir build-debug -L 'smoke|stress' --output-on-failure
```

The `contract` label selects deterministic regressions with 20-second deadlock
limits. These checks use explicit conditions rather than `assert`, so their
verdicts also remain active in Release builds.

| Test | Coverage |
| --- | --- |
| `awaitable_lifecycle_contract` / `_noexc` | Inline completion, worker completion before submit returns, gated delayed completion, submit failure, cancellation before start, cancellation before/after completion; inline and queued continuations. Checks exactly one result, result category, cancellation calls, and destruction after backend exit. |
| `pipeline_lifetime_enabled` / `_disabled` / `_noexc` | Move-only request and shared session cross two executors after public runner/blueprint handles are dropped. Inline/worker/delayed/rejected completion, soft/hard cancellation before/after completion and racing completion, dropping the public controller, nested normal/fast `when_all` across all three-branch completion orders, and normal/fast `when_any` with a late loser or rejected branch. Counts use relaxed atomics; queue publication uses mutexes. Checks exactly-once delivery and balanced resources after backend quiescence. |
| `vaccine_enabled` / `_disabled` / `_noexc` | Read-only success/error/move-only/void inspection, normal/fast/const-view runners, queued handoff and disabled blueprint type/size identity. Failure subprocesses check source position, false/throwing predicates, custom hooks and mandatory termination. `vaccine_disabled_codegen` compares optimized executables (stripped symbols, no build ID) on Linux GCC/Clang. |
| `allocation_failure_contract` | Fail allocation attempts 1 through 3 through the allocator interface; verify recovery on subsequent attempts and no live allocations. Throw from object construction and verify control-block cleanup. |
| `allocation_failure_contract_noexc` | Same allocation failure schedule with exceptions disabled; verifies empty handles and cleanup. |
| `pooling_handoff_contract` / `_noexc` | Cross-thread allocation/return with cache capacities 1, 2, 3, 4, 16 and 512; shared-pool, malloc-fallback and mixed-origin blocks; payload integrity and complete shared-pool reclamation after thread exit. |
| `simple_executor_contract` / `_noexc` | Capacities 1 and 2; nested full-queue dispatch, dispatch before run, concurrent shutdown with an explicitly blocked consumer, repeated shutdown, and draining all admitted work. |
| `void_recovery_contract` / `_noexc` / `_noexc_relaxed` | Void success bypass, error recovery and forwarding, exception matching and replacement; typed conversion and move-only controls; mutable move-only handlers; normal and fast runners. No-exception targets cover both strict and relaxed policy. |
| `end_move_contract` / `_noexc` / `_noexc_relaxed` | Default end preserves success/error, value and ownership for tracked, move-only and void results without copying. Checks normal, fast owning and fast view runners, inline and queued delivery, balanced payload lifetimes, and a separate copy-only fallback control. |
| `gsource_executor_contract` | Capacities 1 and 2; dispatch while owning the context outside the executor callback, nested dispatch from its callback, producer-thread wakeups and delivery. Requires the optional GLib target. |
| `uv_executor_contract` / `_noexc` | Atomic admission/close tickets, bounded MPSC queue, paused publication, producer/closer paused after native wake, owner-only final close, retained rejection and lifetime. Concurrent waves include capacities 1, 2, 3, 7 and 64. TSan runs synchronization cases without the conflicting global allocator replacement. |
| `io_uring_attachment_contract` / `_noexc` | Borrowed ring admission, foreign/stale/duplicate CQEs, errno, normal/fast runners, cancellation retaining owned buffers, reentrant submission and close/drain, including concurrent close while SQ preparation is paused. `io_uring_runtime` / `_noexc` performs a real file read sharing a batch with a host NOP; blocked/unsupported kernels explicitly skip. |
| `dpdk_awaitable_contract` / `_noexc` | Fake RX/TX API: empty RX remains pending, one waiter per queue, cancellation before/during receive, close/drain, move-only packet ownership and zero/partial TX acceptance. Native headers are also compiled; no hardware or PMD runtime validation is implied. |

Enable the I/O attachment targets with `FLUX_FOUNDRY_ENABLE_IO_URING=ON` and/or
`FLUX_FOUNDRY_ENABLE_DPDK=ON`, then run `ctest -L 'io_uring|dpdk'`. See the
[attachment guide](../docs/io_attachments.md) for dependencies and host contracts.

The optional GNU/Clang `io_uring_adapter_rejection_*` and
`dpdk_adapter_rejection_*` tests verify `static_assert` diagnostics in C++14,
with and without exceptions. They require valid controls to compile, then match
the intended messages for 13 io_uring and 19 DPDK cases per mode: missing/wrong
signatures, throwing callbacks/moves/destructors, invalid packet/operation types,
out-of-range capacities and invalid errno adapters. Compile failures caused by
missing dependency headers do not count as successful rejections.

Enable and run GLib coverage on Linux:

```sh
cmake -S . -B build-debug -DFLUX_FOUNDRY_ENABLE_GSOURCE_EXECUTOR=ON
cmake --build build-debug --target flux_foundry_gsource_executor_contract
ctest --test-dir build-debug -L gsource --output-on-failure
```

For GLib, register the executor before concurrent dispatch and stop producers
before destruction. Destroy it on the context owner, or after context dispatch
has stopped. Destruction inside a task, including a nested context iteration,
is supported: the current task finishes and queued tasks are discarded. Callback
state lives in the GSource allocation and is retained until active callbacks
return; no extra state allocation is needed and dispatch does not allocate queue storage. Re-registering an attached
source reports `EALREADY`.

Each enqueue still writes eventfd; the callback rearms after its ten-task budget.
Four-producer wave tests exercise empty transitions and budget rearming at queue
capacities 1, 2 and 64. Callback destruction tests also verify pending captures
are released; sanitizer runs can detect stale callback state or source functions.
Use `G_DEBUG=fatal-warnings` to treat GLib lifecycle diagnostics as failures.

When enqueueing fails on the
owning context thread, dispatch executes inline. This can recurse and does not
preserve FIFO order. Other producers wait for queue space and must not block the
consumer's progress path.

ASan and UBSan are run by the Linux sanitizer CI job. To reproduce locally:

```sh
cmake -S . -B build-sanitizers -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_STANDARD=14 -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined' \
  -DFLUX_FOUNDRY_BUILD_TESTS=ON -DFLUX_FOUNDRY_ENABLE_GSOURCE_EXECUTOR=ON
cmake --build build-sanitizers --parallel 2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build-sanitizers -L 'smoke|stress' --output-on-failure --timeout 300
```

Sanitizer startup failures are infrastructure failures, not passing tests.
MSan additionally needs instrumented C++ dependencies. Linking an MSan test
against the system libstdc++ can report false positives at `std::exception_ptr`
boundaries even in an FF-free program. Use an MSan-built libc++ and libc++abi
for exception-enabled tests; do not suppress the report in FF's error handling.
The [DPDK MSan recheck](../bench/io_adapters/results/2026-11-13/batch-prefix/tests/msan-runtime.md)
records a working isolated runtime build, the minimal reproducer and a negative
control. It covers the fake backend; uninstrumented native I/O libraries need
separate treatment before claiming MSan coverage of those backends.
TSan runs separately in the nightly workflow and includes the deterministic
awaitable lifecycle test. Protocol models are documented in [model/README.md](model/README.md).

These regressions do not exhaust all allocator failure sites, fast-awaitable
schedules, external backends, or long-running production workloads. Allocation
injection here is scoped to `lite_ptr`'s custom allocator and constructor cleanup;
it does not simulate process-wide out-of-memory conditions.

## Poison instrumentation

`poison_enabled`, `poison_disabled`, and `poison_noexc` cover borrowed values,
move-only callbacks, inline/worker completion, and a real simple_executor handoff.
Failure subprocesses check expired borrows, logical buffer reuse, duplicate and
racing completion, rejection, missing completion at quiescence, and callback
entry after owner destruction. `poison_disabled_codegen` compares erased and
plain optimized executables. Existing pipeline lifetime scenarios also instrument
the backend boundary, including cancellation with a legitimate late completion.
See [the API and limitations](../docs/poison.md).

## Repeating realistic lifecycle schedules

The pipeline lifetime executable accepts a repetition count (1–10000). Each
round runs 58 scenarios and reports the concrete runner/awaitable mode and aggregate completion order;
there are no sleep-based correctness assumptions or random seeds to lose.
A simultaneous cancel/completion case intentionally accepts either legal winner,
while still requiring one delivered result and balanced ownership.

```sh
./build-debug/test/flux_foundry_pipeline_lifetime_enabled 100
./build-debug/test/flux_foundry_pipeline_lifetime_disabled 100
./build-debug/test/flux_foundry_pipeline_lifetime_noexc 100
```

Run these on ARM as well as x86. TSan uses the library's existing TSan-aware
atomic paths; successful TSan runs do not establish correctness of every weak
memory ordering in an ordinary build. Keep unsanitized ARM stress coverage.
The nightly TSan workflow runs both enabled and disabled checkpoint variants and
retains failing output, including the round and exact schedule.

## Identity-routed awaitable pooling

`ctest --test-dir <build> -L identity-pool --output-on-failure` runs the new
identity allocator contract, the ordinary/fast pipeline lifetime contracts
with the default policy enabled, their no-exception counterparts, legacy policy controls, and
compile-time rejection cases. See [the policy contract](../docs/identity_pool.md).
