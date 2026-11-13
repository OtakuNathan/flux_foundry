#include <extension/io_uring_awaitable.h>
int main() {
    // No SQEs are prepared here, so installation testing needs no kernel ring.
    io_uring ring{};
    flux_foundry::extension::io_uring_context attached(ring, 0xff30);
    return attached.try_detach() ? 0 : 1;
}
