# libuv executor

`executor/uv_executor.h` exposes `flux_foundry::uv_executor`, a C++14 FF executor
for a caller-owned libuv loop. It originates from the Dynabridge adapter used by
pal-shell-native: `dynabridge/extensions/flux_foundry/uv_executor.h` at commit
`99d363fcff63c205d780cb7ff4ce018213c42357`. The common repository owner authorized
integration under FF's MIT license. Pal and Dynabridge are not dependencies.

This is a loop executor using `uv_async`, not `uv_queue_work` or libuv's global
thread pool. The caller chooses the loop thread. An idle loop waits for events;
there is no polling worker in this adapter. Blocking business work belongs on a
separate worker loop if other I/O must stay responsive.

```cmake
set(FLUX_FOUNDRY_ENABLE_UV_EXECUTOR ON CACHE BOOL "" FORCE)
add_subdirectory(path/to/flux_foundry)
target_link_libraries(my_target PRIVATE flux_foundry::uv_executor)
```

For an installed package built with this option:

```cmake
find_package(flux_foundry CONFIG REQUIRED COMPONENTS uv_executor)
target_link_libraries(my_target PRIVATE flux_foundry::uv_executor)
```

The optional component uses pkg-config's `libuv` package and Threads. The core
FF target does not gain a libuv dependency. The component supports both exception
and `-fno-exceptions` builds, including the core's strict/relaxed compile policies.
Tasks still require `noexcept` in both policies.

For checked initialization, construct `uv_executor(std::nothrow, &loop, 1024, 16)`
on the loop owner and inspect `error()` before use: zero means ready, otherwise
it contains a libuv error (including `UV_EINVAL` for invalid arguments). A failed
executor rejects dispatch, reports closed, and has no borrowed loop. The existing
constructor throws with exceptions enabled and terminates on initialization
failure without exceptions. The nothrow tag makes parameter/libuv initialization
failure explicit; it does not provide recoverable allocation failure for the
standard allocator used during construction. Allocation failure terminates in
no-exception builds.

```cpp
uv_loop_t loop{};
if (uv_loop_init(&loop)) throw std::runtime_error("loop init");
{
    // Construct on the thread which owns and will run the loop.
    flux_foundry::uv_executor executor(&loop, 1024, 16);
    std::thread producer([&] {
        executor.dispatch(flux_foundry::task_wrapper_sbo([]() noexcept {
            // Runs on this loop's owner thread.
        }));
        executor.close();
    });
    uv_run(&loop, UV_RUN_DEFAULT);
    producer.join();
    if (!executor.closed()) std::terminate();
}
if (uv_loop_close(&loop)) std::terminate();
```

The loop must outlive the executor's handle close callback. Initialization and
`uv_run` belong to the same thread; do not run that loop concurrently or
recursively. `loop()` returns the borrowed pointer until close is requested,
then null. A pointer returned before concurrent close is not an admission token.

`try_dispatch(task&&)` returns false without consuming the task for a full queue,
closed admission, or empty task. The bounded ring is allocated at construction;
dispatch does not allocate queue storage. On true, the task runs
exactly once. Tasks must be `noexcept`. Submission from the owner with an empty
queue executes inline and may recurse. Submission with an existing queue joins
its tail; full-queue rejection applies to the owner too, avoiding a wait for its
own progress. The capacity bounds queued tasks, excluding the task executing on
the owner and any inline recursion. The callback drains at most the configured
budget of queued tasks; recursive inline work is not covered by that budget.

FF's `void dispatch(task&&)` cannot report rejection: it terminates on rejected
submission instead of losing a continuation. Use `try_dispatch` at bounded
application ingress, or provision capacity and enforce admission before using
FF `via`. A hard libuv wake error after enqueue also terminates; it cannot be
reported as an unsubmitted task after ownership has transferred.

`close()` is thread-safe and idempotent while the executor object remains alive.
It stops admission, drains accepted tasks, and schedules handle closure on the
owner. It does not block, join a thread, cancel an operation, or close unrelated
loop handles. Follow-up submissions from a draining task are rejected too. The
destructor requests close and retains no obligation to wait; independent state
keeps the handle alive until `uv_close` completes. The caller must still drive
the loop through that callback. Destroying the public C++ object concurrently
with its member calls is invalid, as with other C++ objects.

Compared with the original adapter, this version bounds the queue, adds explicit
rejection, synchronizes close/send, retains handle state independently, drains
without allocating during dispatch or drain, checks wake errors, and exposes completed
closure. Queue state uses mutex synchronization, including with libuv versions
where repeated async notifications coalesce; no one-notification-per-task
assumption is made.

`uv_executor_contract` tests owner affinity/inline nesting, full-queue retained
rejection, dispatch under allocation failure, checked initialization failure,
coalesced notification/budget rearm,
accepted-task drain, concurrent producers/close, destruction before and during a
callback, and normal/fast FF runner continuations. A standalone-header target
checks that including this header does not require preceding FF headers.
`uv_executor_contract_noexc` runs these contracts with exceptions disabled;
the legacy throwing-constructor assertion is specific to the exception build.
