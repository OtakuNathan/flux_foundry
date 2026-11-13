#include "flow/flow.h"
#include "extension/io_uring_awaitable.h"
#include "common.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace flux_foundry;
using namespace flux_foundry::extension;
using probe::require;
using E = flow_async_agg_err_t;

struct operation {
    std::uint64_t id;
    void prepare(io_uring_sqe* sqe) noexcept { io_uring_prep_nop(sqe); }
};
using request = io_uring_request<operation>;
using output = result_t<io_uring_result<operation>, E>;
struct observed {
    std::uint64_t count = 0, sum = 0;
    void record(std::uint64_t id) noexcept { ++count; sum += id; }
};
struct receiver {
    using value_type = output;
    observed* seen;
    void emplace(output&& out) noexcept {
        require(out.has_value() && out.value().result == 0, "NOP awaitable failed");
        seen->record(out.value().operation.id);
    }
};
struct callback_data { observed* seen; std::uint64_t id; };
void completed(void* data, int res, unsigned flags) noexcept {
    require(res == 0 && flags == 0, "NOP callback failed");
    auto* p = static_cast<callback_data*>(data);
    p->seen->record(p->id);
}

// Staged mode measures userspace submission/routing only. No kernel I/O is
// performed: SQE tokens are returned synchronously by this harness after a batch.
template <int Mode>
double measure(bool kernel, std::size_t count, unsigned batch) {
    io_uring ring{};
    std::vector<io_uring_sqe> sqes(batch);
    unsigned head = 0;
    if (kernel) {
        const int rc = io_uring_queue_init(batch, &ring, 0);
        if (rc) { std::fprintf(stderr, "io_uring_queue_init failed: %d\n", rc); std::exit(1); }
    } else {
        ring.sq.sqes = sqes.data();
        ring.sq.khead = &head;
        ring.sq.ring_entries = batch;
        ring.sq.ring_mask = batch - 1;
    }
    double elapsed;
    {
        io_uring_context context(ring, 0xff20, batch);
        auto bp = make_blueprint<request, E>() | await_io_uring<operation>() | end();
        auto shared = make_lite_ptr<decltype(bp)>(std::move(bp));
        observed seen;
        std::vector<callback_data> callbacks(batch);
        const auto start = probe::clock::now();
        for (std::size_t base = 0; base < count; base += batch) {
            for (unsigned i = 0; i < batch; ++i) {
                operation op{base + i + 1};
                if (Mode == 0) {
                    auto* sqe = io_uring_get_sqe(&ring);
                    require(sqe != nullptr, "raw SQ full");
                    op.prepare(sqe);
                    io_uring_sqe_set_data64(sqe, op.id);
                } else if (Mode == 1) {
                    callbacks[i] = {&seen, op.id};
                    require(context.enqueue(op, &completed, &callbacks[i]) == 0, "context SQ full");
                } else if (Mode == 2) {
                    auto runner = make_runner(shared, receiver{&seen});
                    runner(request{&context, std::move(op)});
                } else {
                    auto runner = make_fast_runner(shared, receiver{&seen});
                    runner(request{&context, std::move(op)});
                }
            }
            if (kernel) {
                unsigned submitted = 0;
                while (submitted < batch) {
                    const int rc = io_uring_submit(&ring);
                    if (rc == -EINTR) continue;
                    require(rc > 0, "submit failed or made no progress");
                    submitted += static_cast<unsigned>(rc);
                }
                require(submitted == batch, "unexpected submit count");
            } else {
                head = ring.sq.sqe_tail;
            }
            for (unsigned i = 0; i < batch; ++i) {
                std::uint64_t token;
                int res = 0;
                unsigned flags = 0;
                if (kernel) {
                    io_uring_cqe* cqe;
                    int rc;
                    do { rc = io_uring_wait_cqe(&ring, &cqe); } while (rc == -EINTR);
                    require(rc == 0, "wait_cqe failed");
                    token = cqe->user_data; res = cqe->res; flags = cqe->flags;
                    io_uring_cqe_seen(&ring, cqe);
                } else {
                    token = sqes[i].user_data;
                }
                if (Mode == 0) {
                    require(res == 0 && flags == 0, "raw NOP failed");
                    seen.record(token);
                } else {
                    require(context.complete(token, res, flags), "unroutable completion");
                }
            }
        }
        elapsed = probe::ns(start, probe::clock::now()) / count;
        require(seen.count == count && seen.sum == count * (count + 1) / 2,
                "completion count/checksum mismatch");
        require(context.try_detach(), "context did not drain");
    }
    if (kernel) io_uring_queue_exit(&ring);
    return elapsed;
}

int main(int argc, char** argv) {
    if (argc != 6) {
        std::fprintf(stderr, "usage: uring kernel|staged raw|context|normal|fast COUNT BATCH CPU\n");
        return 2;
    }
    const std::string backend = argv[1], mode = argv[2];
    require(backend == "kernel" || backend == "staged", "unknown backend");
    auto count = std::stoull(argv[3]);
    const unsigned batch = std::stoul(argv[4]);
    require(batch && batch <= 32768 && !(batch & (batch - 1)) && count && count % batch == 0,
            "batch must be a power of two <=32768; count must be a positive multiple");
    probe::pin(std::stoul(argv[5]));
    double (*run)(bool, std::size_t, unsigned) = nullptr;
    if (mode == "raw") run = measure<0>;
    else if (mode == "context") run = measure<1>;
    else if (mode == "normal") run = measure<2>;
    else if (mode == "fast") run = measure<3>;
    require(run != nullptr, "unknown mode");
    run(backend == "kernel", std::min<unsigned long long>(count, 32768), batch);
    const auto ns = run(backend == "kernel", count, batch);
    std::printf("uring,%s,%s,%llu,%u,%.3f\n", argv[1], argv[2], count, batch, ns);
}
