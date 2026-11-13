#include "extension/io_uring_awaitable.h"
void check_io_uring_header(io_uring& ring) {
    flux_foundry::extension::io_uring_context attached(ring, 0xff20);
    attached.try_detach();
}
