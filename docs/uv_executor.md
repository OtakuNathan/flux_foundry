# libuv executor

`executor/uv_executor.h` exposes `flux_foundry::uv_executor`, a C++14 FF executor
for a caller-owned libuv loop. It originates from the adapter in Portal (formerly Dynabridge) used by
pal-shell-native: `dynabridge/extensions/flux_foundry/uv_executor.h` at commit
`99d363fcff63c205d780cb7ff4ce018213c42357`. The common repository owner authorized
integration under FF's MIT license. Pal and Portal are not dependencies.

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
closed admission, or empty task. The bounded MPSC ring is allocated at construction;
dispatch does not allocate queue storage. On true, the task runs
exactly once. Tasks must be `noexcept`. Submission from the owner with an empty
queue executes inline and may recurse. Submission with an existing queue joins
its tail; full-queue rejection applies to the owner too, avoiding a wait for its
own progress. The capacity bounds queued tasks and unpublished reservations, excluding the task executing on
the owner and any inline recursion. The callback drains at most the configured
budget of queued tasks; recursive inline work is not covered by that budget.

### Explicit batch submission

`try_dispatch_batch(task_t* tasks, std::size_t count)` queues a prefix and
returns its length. It acquires one admission ticket and sends one native
notification for the accepted prefix. Each task still uses the existing MPSC
publication protocol. The operation does not allocate queue storage or wait
for space, and it stops at the first empty task or full queue. Null pointers,
zero counts, closed admission and uninitialized executors return zero.

Accepted tasks are moved out of the array; the unaccepted suffix is untouched.
The array must remain valid through the call, and the public executor must
outlive concurrent producer calls. Concurrent batches can interleave at task
boundaries; a batch is not an indivisible transaction or a contiguous global
ordering unit. A batch admitted before concurrent `close()` may finish queuing
its prefix afterward; all accepted tasks are included in the close drain.

Batch submission always queues, including on the owner with an empty queue.
It never calls user tasks inline and never recursively executes a batch. This
avoids callback reentrancy from an owner-thread batch call. With a concurrent
producer, the owner may execute accepted tasks and free capacity before the
batch call returns. A callback may enqueue another batch; that work joins the queue
and remains subject to the callback budget. The owner must return to the event
loop to make space instead of spinning on an unaccepted suffix. Single-task
`try_dispatch` keeps its existing owner-inline behavior.

```cpp
flux_foundry::uv_executor::task_t tasks[] = {
    flux_foundry::uv_executor::task_t([]() noexcept { /* first task */ }),
    flux_foundry::uv_executor::task_t([]() noexcept { /* second task */ })
};
const std::size_t accepted = executor.try_dispatch_batch(tasks, 2);
// tasks[0..accepted) were transferred; retain/retry the remaining suffix
// according to the application's bounded-ingress policy.
```

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

`try_dispatch` borrows the internal state pointer instead of copying its
`shared_ptr` on every submission. Concurrent producers must keep the public
object alive throughout their calls. An owner-inline task may destroy that
object: the native handle's independent owner retains the state until its close
callback, and recursive `uv_run` is prohibited. The admission ticket still
covers the complete native send. This removes a shared reference-count update
pair without changing queue publication or the close protocol.

The close protocol follows `simple_executor`'s combined shutdown-bit/pending-ticket
state. `utility/close_gate.h` implements that admission rule with CAS. Setting
the stop bit and acquiring a ticket are ordered by the same atomic; a request
either enters before close or is rejected without consuming its task. The gate
requires always-lock-free `std::atomic<size_t>`. A C++14 `static_assert` selects
the standard `ATOMIC_*_LOCK_FREE` macro for `size_t`'s actual unsigned type and
rejects targets without that compile-time guarantee. Queue task move construction,
move assignment and destruction are also statically required to be nonthrowing.

There are two conditions for native handle closure: the queue is empty **and**
all admitted senders have returned their tickets. A producer keeps its ticket
through `uv_async_send`, even when the loop has already consumed its task. The
winning closer atomically sets stop and acquires its own notification ticket,
sends once, then releases it. Later close calls do not send again. Only the loop
calls `uv_close`, after both conditions hold. `closed()` becomes true in the
native close callback, not at the admission stop transition.

An async callback may run before its sender releases that ticket. During close,
the loop rearms a notification while senders remain; the last producer never
tries to send after releasing its ticket. This avoids both a missed final wake
and a send racing handle reclamation. Draining remains asynchronous: a paused
producer or long-running task can delay completed shutdown. The owner does not
spin waiting inside a callback, but may run repeated close-drain callbacks.

The MPSC ring reserves bounded occupancy before assigning a slot. Occupancy
includes producers that have not published their task yet. Each slot has an
atomic ready flag; release/acquire publishes the task and publishes slot reuse.
The tail CAS also uses acquire/release: a producer can reserve capacity before
its eventual slot is freed, so later reservations must pass that consumer-release
ordering through the tail to the producer that finally claims the slot.
The single consumer stops at an unpublished head rather than overtaking it or
blocking the loop. Its publisher will wake the loop after publication. All
positive runtime capacities remain supported, including 1 and non-powers of two.
Indices wrap within the ring, so there is no lifetime limit from sequence overflow.

Each successful queued single submission or nonempty accepted batch sends a notification;
libuv coalesces these.
The callback rearms when its budget leaves a ready head or closing senders remain.
A full fence precedes native sends when the linked libuv runtime is older than
1.53.0. From 1.53.0, libuv guarantees sequential consistency for async sends and
callbacks, including coalesced sends, so the redundant caller fence is omitted.
The choice is cached at construction using `uv_version()`, not the potentially
different header version. The optional final constructor argument
`uv_executor::wake_fence_policy::always_fence` retains the conservative fence
on every version, for example for a matched performance control; the default is
`runtime_compatible`. Neither option removes the compatibility fence from old
runtimes. Queue publication and gate memory orders are unchanged. See the
[libuv async memory-ordering notes](https://docs.libuv.org/en/v1.x/async.html).
These changes remove FF's mutex from queue/admission/close, but do not claim
lock-free execution of user callbacks, allocation, or native libuv internals.

`uv_executor_contract` tests owner affinity/inline nesting, full-queue retained
rejection, dispatch under allocation failure, checked initialization failure,
coalesced notification/budget rearm,
accepted-task drain, concurrent producers/close, destruction before and during a
callback, normal/fast FF runner continuations, and destruction inside a direct
or nested owner-inline dispatch. A standalone-header target
checks that including this header does not require preceding FF headers.
Repeated four-producer waves cover draining to empty and rearming at budgets
1, 3, 16 and 64, with retained tasks retried after bounded rejection.
Additional waves exercise capacities 1, 2, 3 and 7. Deterministic barriers pause
an unpublished head, a producer after native send, and the closer after native
send. These verify that empty queue alone cannot authorize handle closure, and
that admitted work is not lost or reordered behind a paused publisher.
`uv_executor_contract_noexc` runs these contracts with exceptions disabled;
the legacy throwing-constructor assertion is specific to the exception build.
TSan builds omit only the global new/delete fault-injection replacement, which
conflicts with TSan's allocator interceptors; regular and UBSan builds exercise it.
Batch contracts additionally cover partial acceptance, an empty task within the
input, enqueue-only owner behavior, exactly one send per accepted batch,
reentrant batches, callback-triggered executor destruction, concurrent close,
unpublished reservations and admission held through a paused final send.
Both automatic and forced compatibility-fence policies are exercised.
