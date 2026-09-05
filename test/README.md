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
| `awaitable_lifecycle_contract` | Inline completion, worker completion before submit returns, gated delayed completion, submit failure, cancellation before start, cancellation before/after completion; inline and queued continuations. Checks exactly one result, result category, cancellation calls, and destruction after backend exit. |
| `allocation_failure_contract` | Fail allocation attempts 1 through 3 through the allocator interface; verify recovery on subsequent attempts and no live allocations. Throw from object construction and verify control-block cleanup. |
| `allocation_failure_contract_noexc` | Same allocation failure schedule with exceptions disabled; verifies empty handles and cleanup. |
| `simple_executor_contract` / `_noexc` | Capacities 1 and 2; nested full-queue dispatch, dispatch before run, concurrent shutdown with an explicitly blocked consumer, repeated shutdown, and draining all admitted work. |
| `gsource_executor_contract` | Capacities 1 and 2; dispatch while owning the context outside the executor callback, nested dispatch from its callback, producer-thread wakeups and delivery. Requires the optional GLib target. |

Enable and run GLib coverage on Linux:

```sh
cmake -S . -B build-debug -DFLUX_FOUNDRY_ENABLE_GSOURCE_EXECUTOR=ON
cmake --build build-debug --target flux_foundry_gsource_executor_contract
ctest --test-dir build-debug -L gsource --output-on-failure
```

For GLib, register the executor before concurrent dispatch and keep registration
and destruction synchronized with producers. When enqueueing fails on the
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
TSan runs separately in the nightly workflow and includes the deterministic
awaitable lifecycle test. Protocol models are documented in [model/README.md](model/README.md).

These regressions do not exhaust all allocator failure sites, fast-awaitable
schedules, external backends, or long-running production workloads. Allocation
injection here is scoped to `lite_ptr`'s custom allocator and constructor cleanup;
it does not simulate process-wide out-of-memory conditions.
