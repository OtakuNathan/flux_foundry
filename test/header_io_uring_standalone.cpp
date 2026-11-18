#include "extension/io_uring_awaitable.h"
void check_io_uring_header(io_uring& ring) {
    struct nop { void prepare(io_uring_sqe* sqe) noexcept { io_uring_prep_nop(sqe); } };
    auto fast = flux_foundry::await_io_uring_fast<nop>();
    (void)fast;
    flux_foundry::extension::io_uring_context attached(ring, 0xff20);
    attached.try_detach();
}
