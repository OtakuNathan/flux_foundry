#include "extension/dpdk_awaitable.h"
// Compile the real DPDK API path, independent of the mock contract tests.
void check_dpdk_header() {
    flux_foundry::extension::dpdk_rx_context<> attached(0, 0);
    attached.poll();
    auto batch = flux_foundry::extension::dpdk_packet_batch<>::receive(0, 0);
    batch.transmit(0, 0);
    attached.try_detach();
}
