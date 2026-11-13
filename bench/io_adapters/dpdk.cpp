// Actual DPDK net_ring PMD, single-core hot-mbuf loopback. No physical NIC.
#include "flow/flow.h"
#include "extension/dpdk_awaitable.h"
#include "common.h"
#include <rte_eal.h>
#include <rte_eth_ring.h>
#include <rte_version.h>
#include <array>
#include <cstring>
#include <string>
#include <vector>

using namespace flux_foundry;
using namespace flux_foundry::extension;
using probe::require;
using E = flow_async_agg_err_t;

struct observed {
    std::uint16_t port;
    std::uint64_t packets = 0, sum = 0;
    void record(rte_mbuf* packet) noexcept {
        require(rte_pktmbuf_pkt_len(packet) == 64, "packet length mismatch");
        sum += *rte_pktmbuf_mtod(packet, unsigned char*);
        ++packets;
    }
    template <typename Batch>
    void consume(Batch&& batch) noexcept {
        const auto count = batch.size();
        for (std::uint16_t i = 0; i < count; ++i) record(batch[i]);
        require(batch.transmit(port, 0) == count && !batch.size(), "partial loopback TX");
    }
};

template <unsigned Size>
struct receiver {
    using value_type = result_t<dpdk_packet_batch<Size>, E>;
    observed* seen;
    void emplace(value_type&& out) noexcept {
        require(out.has_value(), "DPDK awaitable failed");
        seen->consume(std::move(out).value());
    }
};

template <unsigned Size, unsigned Mode>
double measure(std::uint16_t port, std::size_t bursts) {
    using context_type = dpdk_rx_context<Size>;
    using batch_type = dpdk_packet_batch<Size>;
    context_type context(port, 0);
    observed seen{port};
    auto bp = make_blueprint<context_type*, E>() | await_dpdk_rx<Size>() | end();
    auto shared = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto callback = [](void* data, batch_type&& batch, int error) noexcept {
        require(!error, "DPDK context failed");
        static_cast<observed*>(data)->consume(std::move(batch));
    };
    auto canceled = [](void*) noexcept { return false; };
    const auto start = probe::clock::now();
    for (std::size_t i = 0; i < bursts; ++i) {
        if (Mode == 0) {
            rte_mbuf* packets[Size];
            const auto n = rte_eth_rx_burst(port, 0, packets, Size);
            require(n == Size, "raw RX burst incomplete");
            for (unsigned j = 0; j < n; ++j) seen.record(packets[j]);
            require(rte_eth_tx_burst(port, 0, packets, n) == n, "raw TX burst incomplete");
        } else if (Mode == 1) {
            auto batch = batch_type::receive(port, 0);
            require(batch.size() == Size, "RAII RX burst incomplete");
            seen.consume(std::move(batch));
        } else if (Mode == 2) {
            require(context.enqueue(callback, canceled, &seen) == 0, "context enqueue failed");
            require(context.poll(), "context did not receive");
        } else if (Mode == 3) {
            auto runner = make_runner(shared, receiver<Size>{&seen});
            runner(&context);
            require(context.poll(), "normal awaitable did not receive");
        } else {
            auto runner = make_fast_runner(shared, receiver<Size>{&seen});
            runner(&context);
            require(context.poll(), "fast awaitable did not receive");
        }
    }
    const auto elapsed = probe::ns(start, probe::clock::now()) / bursts;
    require(seen.packets == bursts * Size && seen.sum == bursts * Size * (Size + 1) / 2,
            "packet count/checksum mismatch");
    require(context.try_detach(), "context did not drain");
    return elapsed;
}

template <unsigned Size>
void run_cases(std::uint16_t port, rte_mempool* pool, std::size_t bursts, unsigned repeats) {
    std::array<rte_mbuf*, Size> packets;
    require(rte_pktmbuf_alloc_bulk(pool, packets.data(), Size) == 0, "mbuf allocation failed");
    for (unsigned i = 0; i < Size; ++i) {
        auto* data = rte_pktmbuf_append(packets[i], 64);
        require(data != nullptr, "mbuf append failed");
        std::memset(data, i + 1, 64);
    }
    require(rte_eth_tx_burst(port, 0, packets.data(), Size) == Size, "seed TX failed");
    double (*runs[])(std::uint16_t, std::size_t) = {
        measure<Size, 0>, measure<Size, 1>, measure<Size, 2>, measure<Size, 3>, measure<Size, 4>};
    const char* names[] = {"raw", "batch", "context", "normal", "fast"};
    for (unsigned repeat = 0; repeat < repeats; ++repeat) {
        for (unsigned position = 0; position < 5; ++position) {
            const auto mode = (position + repeat) % 5;
            runs[mode](port, 16384);
            const auto ns = runs[mode](port, bursts);
            std::printf("%u,dpdk,net_ring,%s,%zu,%u,%.3f,%.3f\n", repeat, names[mode],
                        bursts, Size, ns, ns / Size);
            std::fflush(stdout);
        }
    }
    require(rte_eth_rx_burst(port, 0, packets.data(), Size) == Size, "final drain failed");
    for (auto* packet : packets) rte_pktmbuf_free(packet);
}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: dpdk BURSTS REPEATS CPU\n");
        return 2;
    }
    const auto bursts = std::stoull(argv[1]);
    const unsigned repeats = std::stoul(argv[2]);
    const auto cpu = std::string(argv[3]);
    require(bursts && repeats, "bursts/repeats must be positive");
    std::vector<std::string> args = {"ff-dpdk-probe", "-l", cpu, "--no-huge", "--no-pci",
                                    "--no-shconf", "--no-telemetry", "-m", "64", "--log-level=warning"};
    std::vector<char*> cargs;
    for (auto& arg : args) cargs.push_back(&arg[0]);
    require(rte_eal_init(static_cast<int>(cargs.size()), cargs.data()) >= 0, "EAL initialization failed");
    std::fprintf(stderr, "%s; software net_ring loopback, 64-byte hot mbufs\n", rte_version());
    auto* pool = rte_pktmbuf_pool_create("ff_bench_pool", 4095, 0, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    require(pool != nullptr, "mempool creation failed");
    auto* ring = rte_ring_create("ff_bench_ring", 1024, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
    require(ring != nullptr, "ring creation failed");
    const int p = rte_eth_from_ring(ring);
    require(p >= 0, "ring PMD creation failed");
    const auto port = static_cast<std::uint16_t>(p);
    rte_eth_conf config{};
    require(rte_eth_dev_configure(port, 1, 1, &config) == 0, "port configure failed");
    require(rte_eth_rx_queue_setup(port, 0, 256, rte_socket_id(), nullptr, pool) == 0, "RX queue setup failed");
    require(rte_eth_tx_queue_setup(port, 0, 256, rte_socket_id(), nullptr) == 0, "TX queue setup failed");
    require(rte_eth_dev_start(port) == 0, "port start failed");
    std::puts("repeat,backend,path,mode,bursts,batch,ns_per_burst,ns_per_packet");
    run_cases<1>(port, pool, bursts, repeats);
    run_cases<32>(port, pool, bursts, repeats);
    require(rte_ring_count(ring) == 0 && rte_mempool_avail_count(pool) == 4095, "ring/mbuf leak");
    require(rte_eth_dev_stop(port) == 0, "port stop failed");
    require(rte_eth_dev_close(port) == 0, "port close failed");
    rte_ring_free(ring);
    rte_mempool_free(pool);
    require(rte_eal_cleanup() == 0, "EAL cleanup failed");
}
