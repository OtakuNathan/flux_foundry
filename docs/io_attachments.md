# Awaitables attached to host I/O contexts

The optional headers `extension/io_uring_awaitable.h` and
`extension/dpdk_awaitable.h` adapt application-owned I/O resources at the
awaitable boundary. Like `uv_executor`, they borrow resources from the host.
They create no runtime, executor thread, ring, EAL instance, port or mempool.
An application can keep the attachment beside its global I/O context or use
one per loop. FF does not impose a process-wide singleton.

```cmake
set(FLUX_FOUNDRY_ENABLE_IO_URING ON CACHE BOOL "" FORCE)
set(FLUX_FOUNDRY_ENABLE_DPDK ON CACHE BOOL "" FORCE)
add_subdirectory(path/to/flux_foundry)
target_link_libraries(app PRIVATE flux_foundry::io_uring flux_foundry::dpdk)
```

Each component is independently optional. Both are header-only, C++14, and
support exception and no-exception builds. The core target gains no external
dependency. Installed packages expose `io_uring` and `dpdk` components:

```cmake
find_package(flux_foundry CONFIG REQUIRED COMPONENTS io_uring dpdk)
target_link_libraries(app PRIVATE flux_foundry::io_uring flux_foundry::dpdk)
```

Dependencies are discovered through pkg-config (`liburing`, `libdpdk`). Local
validation used liburing 2.9 and DPDK 24.11.4 on Linux ARM64. With exceptions,
the default error type is `std::exception_ptr`; native errno failures become
`std::system_error`. Without exceptions it is `std::error_code`; supply FF's
normal error-hook specializations as described by the existing flow contracts.
Other error types need a `extension::detail::io_error<E>` specialization as
well as FF's error hooks and exception conversion requirements.

## io_uring: attach, stage SQEs, route CQEs

```cpp
#include "flow/flow.h"
#include "extension/io_uring_awaitable.h"
#include <array>

using namespace flux_foundry;
using namespace flux_foundry::extension;

struct read_operation {
    int fd;
    std::array<char, 4096> buffer{};
    void prepare(io_uring_sqe* sqe) noexcept {
        io_uring_prep_read(sqe, fd, buffer.data(), buffer.size(), 0);
    }
};

// ring was initialized by the host. The host reserves top-16-bit tag 0xff01
// exclusively for this attachment; 256 is the outstanding request limit.
io_uring_context attached(ring, 0xff01, 256);
using request = io_uring_request<read_operation>;
auto bp = make_blueprint<request, flow_async_agg_err_t>()
    | await_io_uring<read_operation>()
    | transform([](io_uring_result<read_operation> read) noexcept {
        // read.result is the actual byte count, possibly short or zero.
        // read.operation.buffer still owns the bytes here.
        return read.result;
    })
    | end();
auto runner = make_runner(make_lite_ptr<decltype(bp)>(std::move(bp)));
runner(request{&attached, read_operation{fd, {}}});

// The host submits its entire SQ batch, including requests from other users.
int submitted = io_uring_submit(&ring);
// Handle submission errors/short submissions in the host's normal policy.
// They DO NOT revoke FF's accepted requests or release their buffers.
```

An operation must be an unqualified object type with nonthrowing move construction
and destruction, and provide `void prepare(io_uring_sqe*) noexcept`, callable on
an lvalue. Expression-based traits and `static_assert` check the signature,
exact void return, and exception specification; overloads are supported.
FF moves it into the awaitable before
calling `prepare`, so addresses of its owned buffers remain stable until the
terminal CQE. Success returns `io_uring_result<Operation>` with the operation,
CQE result and flags. A negative CQE result becomes an error. Borrowed fds and
external buffers must remain valid until the original request is completed,
even if the flow was canceled.

`submit()` stages an SQE and registers its completion token. It does not call
`io_uring_submit`. Full SQ/attachment or closed admission returns a submission
failure without registering a callback. Once accepted, the host must submit
and drain the original request. A failed/short flush is not a rollback. Host
shutdown must resolve pending I/O before destroying the attachment or ring.

The host's CQ loop copies fields and advances the CQ **before** routing them:

```cpp
io_uring_cqe* cqe = nullptr;
while (io_uring_peek_cqe(&ring, &cqe) == 0) {
    const std::uint64_t token = cqe->user_data;
    const int result = cqe->res;
    const unsigned flags = cqe->flags;
    io_uring_cqe_seen(&ring, cqe);
    if (!attached.complete(token, result, flags)) {
        host_complete(token, result, flags);
    }
}
```

`complete()` never consumes the CQ itself. It recognizes the reserved tag,
slot and generation; foreign, stale and duplicate tokens return false without
being dereferenced. The host must coordinate tag allocation with **all** other
ring users, including other FF attachments and reattachments. A host using all
64 bits for unpartitioned identifiers must first establish a routing convention.
Capacity is 1..65536. Slots are recycled before invoking the continuation, so
reentrant submission works. Generation wrap retires a slot rather than allowing
stale-token aliasing. Registration and completion allocate no registry storage;
the table is allocated once at attachment construction. FF awaitable allocation
and the operation's own storage follow their existing allocation policies.

`close()` atomically stops new requests and may be called from another thread.
The admission rule follows `simple_executor`: a stop bit and pending ticket
count share one CAS domain (`utility/close_gate.h`). Admission takes a ticket
before preparing an SQE. Rejection returns it immediately; accepted work keeps
it until the original terminal CQE, including through short/failed submission
and logical cancellation. A request admitted before close may finish preparing
after close, and is still part of the required drain. The gate requires
always-lock-free `std::atomic<size_t>`; unsupported targets are rejected by
`static_assert`, using the corresponding C++14 `ATOMIC_*_LOCK_FREE` macro.

`try_detach()` also closes admission and returns
false while requests remain. Continue flushing/routing until `closed()` or
successful detach, then destroy the attachment. It never calls
`io_uring_queue_exit`. Destruction with outstanding requests terminates as a
lifetime-contract violation. Construction can fail on invalid capacity or
allocation exhaustion; no-exception builds terminate in those cases.

This first adapter supports single-shot requests with one terminal CQE.
Multishot, zero-copy send notifications, CQE-skip-success, linked chains and
buffer-selection ownership are outside its contract. Link/skip/buffer-select flags and
observed `IORING_CQE_F_MORE` fail fast. Flow cancellation is logical: it may
deliver a cancellation error immediately, but does not issue a kernel cancel.
The backend reference retains the operation until the original CQE arrives.
Do not use flow completion as permission to free borrowed I/O buffers.

`close()`, `pending()` and `closed()` may be concurrent with the owner loop while
the attachment remains alive. SQ preparation, CQ routing, `try_detach()` and
destruction still belong to the serialized owner. Atomic shutdown does not make
liburing's SQ or the slot registry multi-producer. Close does not wake a sleeping
host loop; arrange that notification through the host's existing mechanism.
Controller cancellation may also originate on another thread.
Use an existing executor to enter that loop before the await node; an executor
passed to `await_io_uring<Operation>(&executor)` selects where the continuation
resumes, not where SQ preparation runs. Host CQ overflow prevention/draining,
flush recovery, fd ownership and scheduling remain host responsibilities.

## DPDK: await the next RX burst

```cpp
#include "extension/dpdk_awaitable.h"

using namespace flux_foundry;
using namespace flux_foundry::extension;

// Host already initialized EAL and configured/started this RX queue.
dpdk_rx_context<32> attached(port_id, queue_id);
auto bp = make_blueprint<dpdk_rx_context<32>*, flow_async_agg_err_t>()
    | await_dpdk_rx<32>()
    | transform([](dpdk_packet_batch<32> packets) noexcept {
        // Inspect packets[i], or transmit them to a configured TX queue.
        return packets.size();
        // Remaining owned mbufs (including chains) are freed automatically.
    })
    | end();
auto runner = make_runner(make_lite_ptr<decltype(bp)>(std::move(bp)));
runner(&attached);

// Called from the host poll loop alongside its other work:
attached.poll();
```

One attachment admits one pending RX burst waiter; a second waiter receives a
submission failure. Multiple simultaneous consumers of the same RX queue are
not supported. `poll()` does at most one RX burst, makes no RX call without a
waiter, and leaves an empty RX pending. The host must validate the port, queue,
PMD-specific burst-size requirements and mempool lifetime. A zero RX burst is
not an errno channel or a reliable link-state indication.

`dpdk_packet_batch<N>` owns each received mbuf with move-only RAII. It frees
remaining mbufs with `rte_pktmbuf_free`. The mempool must outlive all batches,
including those moved to another executor. Respect the configured pool and
queue threading constraints when selecting that executor.
Only indices below `size()` contain initialized, owned packet pointers. Moving
a batch copies that prefix; unused capacity is not initialized or inspected.

Batch capacity is statically constrained to 1..65535 before conversion to the
native 16-bit burst count. Custom `Api` policies must name an unqualified object
`packet_type` and provide compatible static calls: `void free(packet_type*)`,
`uint16_t rx(uint16_t, uint16_t, packet_type**, uint16_t)` and the same signature
for `tx`, all `noexcept`. Missing members, wrong result types and potentially
throwing calls receive explicit `static_assert` diagnostics when the batch or
context is instantiated. Both awaitables also require the errno adapter
`extension::detail::io_error<E>::make(int)` to return exactly `E` and be `noexcept`.

`packets.transmit(port, queue)` calls `rte_eth_tx_burst` once, returns the number
accepted, and removes only that prefix from the batch. Accepted packets belong
to the PMD; the unsent tail stays owned for retry or destruction. This is a
synchronous burst operation, not a wire-delivery awaitable. PMD acceptance does
not establish that packets reached a peer.

Flow cancellation marks the waiter atomically. The host must still call
`poll()` to release its backend reference; this works even without traffic.
Packets received concurrently with logical cancellation are discarded through
RAII. `close()` stops admission and the next poll completes a pending waiter
with cancellation. `try_detach()` closes and returns false until it is drained.
No port or EAL shutdown is performed. Destruction with a pending waiter
terminates. Calls to the context itself remain serialized on the host loop.

## Validation

The optional CTest targets cover bounded admission, stale/foreign CQEs,
reentrant submission, close/drain, cancellation with retained buffers, normal
and fast runners, empty RX, canceled RX without traffic, mbuf cleanup and
zero/partial TX acceptance. Both exception and strict no-exception builds run.
Standalone header targets compile the native API paths.

GNU/Clang `io_uring_adapter_rejection_*` and `dpdk_adapter_rejection_*` tests
compile a valid control first, then reject 13 io_uring and 19 DPDK misuse cases
in both exception and strict no-exception modes. Each rejection must contain
the intended contract diagnostic; an unrelated compiler failure does not pass.

`io_uring_runtime` additionally performs a real file read and shares the ring
with an unrelated host NOP. Unsupported/blocked kernels report CTest skip 77;
other initialization failures fail the test. DPDK contract tests inject a fake
burst API and do not claim PMD, NIC, hugepage or performance validation.

```bash
cmake -S . -B build/io -DFLUX_FOUNDRY_ENABLE_IO_URING=ON \
  -DFLUX_FOUNDRY_ENABLE_DPDK=ON
cmake --build build/io
ctest --test-dir build/io -L 'io_uring|dpdk' --output-on-failure
```

Native API references: [liburing submission contract](https://github.com/axboe/liburing/blob/master/man/io_uring_submit.3)
and [DPDK RX/TX burst API](https://doc.dpdk.org/api/rte__ethdev_8h.html).
