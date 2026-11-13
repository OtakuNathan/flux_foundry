#include <extension/dpdk_awaitable.h>
int main() {
    // No traffic is polled here: this checks the exported dependency flags.
    flux_foundry::extension::dpdk_rx_context<> attached(0, 0);
    return attached.try_detach() ? 0 : 1;
}
