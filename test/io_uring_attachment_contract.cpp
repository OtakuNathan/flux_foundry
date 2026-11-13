#include "lifecycle_test_support.h"
#include "extension/io_uring_awaitable.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <atomic>
#include <thread>
#include <unistd.h>

using namespace flux_foundry;
using namespace flux_foundry::extension;
using E = lifecycle_test::error;

namespace {
int failures = 0;
void check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "OK" : "FAIL", name);
    if (!ok) ++failures;
}

// Only models userspace SQ storage; no syscalls or emulated kernel success.
struct staged_ring {
    io_uring ring{};
    io_uring_sqe sqes[8]{};
    unsigned head = 0;
    staged_ring() {
        ring.sq.sqes = sqes;
        ring.sq.khead = &head;
        ring.sq.ring_entries = 8;
        ring.sq.ring_mask = 7;
    }
    std::uint64_t last_token() const { return sqes[(ring.sq.sqe_tail - 1) & 7].user_data; }
};
struct payload {
    int* destroyed;
    char byte = 0;
    ~payload() { ++*destroyed; }
};
struct operation {
    std::unique_ptr<payload> buffer;
    int fd = -2;
    void prepare(io_uring_sqe* sqe) noexcept {
        if (fd == -2) io_uring_prep_nop(sqe);
        else io_uring_prep_read(sqe, fd, &buffer->byte, 1, 0);
    }
};
using request = io_uring_request<operation>;
using output = result_t<io_uring_result<operation>, E>;
struct observed { int calls = 0, values = 0, result = -1, error = 0; char byte = 0; };
struct receiver {
    using value_type = output;
    observed* seen;
    void emplace(output&& out) noexcept {
        ++seen->calls;
        if (out.has_value()) {
            ++seen->values;
            seen->result = out.value().result;
            seen->byte = out.value().operation.buffer->byte;
        } else {
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            try { std::rethrow_exception(out.error()); }
            catch (const std::system_error& e) { seen->error = e.code().value(); }
            catch (...) { seen->error = -1; }
#else
            seen->error = out.error().value();
#endif
        }
    }
};
request make_request(io_uring_context& ctx, int& destroyed, int fd = -2) {
    return {&ctx, operation{std::unique_ptr<payload>(new payload{&destroyed, 0}), fd}};
}

template <typename Awaitable = io_uring_awaitable<operation, E>>
void flow_case(bool cancel_first, int result, bool fast) {
    staged_ring ring;
    io_uring_context ctx(ring.ring, 0xff10, 1);
    observed seen;
    int destroyed = 0;
    auto control = make_lite_ptr<flow_controller>();
    auto bp = make_blueprint<request, E>() | await<Awaitable>() | end();
    auto drive = [&](auto& runner) {
        runner(make_request(ctx, destroyed));
        check(ctx.pending() == 1 && !seen.calls && !destroyed, "staging retains operation; no premature completion");
        auto token = ring.last_token();
        check(!ctx.complete(token ^ (std::uint64_t(1) << 48), 0, 0), "foreign CQE remains host-owned");
        if (cancel_first) {
            control->cancel();
            check(seen.calls == 1 && !seen.values && !destroyed, "cancel delivers once and retains backend buffers");
        }
        check(!ctx.try_detach(), "detach refuses outstanding requests");
        check(ctx.complete(token, result, 0), "terminal CQE routed");
        check(!ctx.complete(token, result, 0), "duplicate CQE ignored");
        check(seen.calls == 1 && seen.values == (!cancel_first && result >= 0), "exactly one flow outcome");
        if (!cancel_first && result < 0) check(seen.error == -result, "negative CQE preserves errno");
        check(destroyed == 1 && ctx.closed() && ctx.try_detach(), "draining releases operation and permits detach");
    };
    if (fast) {
        auto runner = make_fast_runner(make_lite_ptr<decltype(bp)>(std::move(bp)), receiver{&seen});
        drive(runner);
    } else {
        flow_runner<decltype(bp), receiver> runner(
            make_lite_ptr<decltype(bp)>(std::move(bp)), control, receiver{&seen});
        drive(runner);
    }
}

template <typename Awaitable = io_uring_awaitable<operation, E>>
void admission_case() {
    staged_ring ring;
    io_uring_context ctx(ring.ring, 0xff11, 1);
    int destroyed = 0;
    observed first, rejected;
    auto bp = make_blueprint<request, E>() | await<Awaitable>() | end();
    auto shared = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = make_runner(shared, receiver{&first});
    auto other = make_runner(shared, receiver{&rejected});
    runner(make_request(ctx, destroyed));
    auto old = ring.last_token();
    other(make_request(ctx, destroyed));
    check(rejected.calls == 1 && !rejected.values && destroyed == 1 && ring.ring.sq.sqe_tail == 1,
          "full attachment rejects without consuming SQE or leaking buffers");
    check(ctx.complete(old, 0, 0), "first request completes");
    auto second = make_runner(shared, receiver{&first});
    second(make_request(ctx, destroyed));
    auto current = ring.last_token();
    check(old != current && !ctx.complete(old, 0, 0), "slot reuse rejects stale generation");
    check(ctx.complete(current, 0, 0), "reused slot completes");
    ring.ring.sq.sqe_tail = 8;
    auto full_sq = make_runner(shared, receiver{&rejected});
    full_sq(make_request(ctx, destroyed));
    check(rejected.calls == 2 && ctx.pending() == 0 && destroyed == 4, "full SQ rejection rolls back backend reference");
    ctx.close();
    auto closed = make_runner(shared, receiver{&rejected});
    closed(make_request(ctx, destroyed));
    check(rejected.calls == 3 && ctx.closed() && destroyed == 5, "close rejects new requests");
}

void terminal_cancel_waits_for_cqe() {
    staged_ring ring;
    io_uring_context ctx(ring.ring, 0xff15, 1);
    int destroyed = 0;
    observed seen;
    auto control = make_lite_ptr<flow_controller>();
    auto bp = make_blueprint<request, E>() | await_io_uring_fast<operation, E>() | end();
    flow_runner<decltype(bp), receiver> runner(make_lite_ptr<decltype(bp)>(std::move(bp)), control, receiver{&seen});
    runner(make_request(ctx, destroyed));
    control->cancel();
    check(ctx.pending() == 1 && !destroyed && !seen.calls,
          "fast node has no early controller cancellation; terminal CQE owns buffer lifetime");
    ctx.close();
    check(ctx.complete(ring.last_token(), 0, 0) && seen.calls == 1 && destroyed == 1 && ctx.closed(),
          "fast node drains original CQE after close and controller request");
}

template <typename F>
struct callback_receiver {
    using value_type = output;
    F callback;
    void emplace(output&& out) noexcept { callback(std::move(out)); }
};
template <typename F>
callback_receiver<F> make_callback_receiver(F f) noexcept { return {std::move(f)}; }

void terminal_reentrant_and_lifetime() {
    staged_ring ring;
    std::unique_ptr<io_uring_context> ctx(new io_uring_context(ring.ring, 0xff16, 1));
    int destroyed = 0;
    observed second;
    std::unique_ptr<payload> held;
    auto bp = make_blueprint<request, E>() | await_io_uring_fast<operation, E>() | end();
    auto first_receiver = make_callback_receiver([&](output&& out) noexcept {
        check(out.has_value() && !ctx->pending(), "fast CQE frees registry slot before continuation");
        held = std::move(out.value().operation.buffer);
        auto next = make_fast_runner_view(bp, receiver{&second});
        next(make_request(*ctx, destroyed));
    });
    auto first = make_fast_runner_view(bp, first_receiver);
    first(make_request(*ctx, destroyed));
    const auto first_token = ring.last_token();
    check(ctx->complete(first_token, 0, 0) && held && !destroyed && ctx->pending() == 1,
          "fast continuation can resubmit into capacity-one registry and retain result buffers");
    check(!ctx->complete(first_token, 0, 0), "reentrant fast node rejects stale original token");
    check(ctx->complete(ring.last_token(), 0, 0) && second.calls == 1 && destroyed == 1,
          "reentrant fast request completes and releases its own buffer once");
    held.reset();
    check(destroyed == 2, "result owner controls retained buffer lifetime");

    auto destroy_receiver = make_callback_receiver([&](output&& out) noexcept {
        check(out.has_value() && !ctx->pending(), "terminal callback sees drained attachment");
        held = std::move(out.value().operation.buffer);
        ctx.reset();
    });
    auto last = make_fast_runner_view(bp, destroy_receiver);
    last(make_request(*ctx, destroyed));
    auto* attachment = ctx.get();
    check(attachment->complete(ring.last_token(), 0, 0) && !ctx && held && destroyed == 2,
          "fast callback may destroy drained attachment while keeping owned result alive");
    held.reset();
    check(destroyed == 3, "destruction from fast callback leaves no backend ownership");
}

void terminal_deferred_receiver() {
    struct queued_executor {
        task_wrapper_sbo pending;
        void dispatch(task_wrapper_sbo&& task) noexcept { pending = std::move(task); }
    } executor;
    staged_ring ring;
    observed seen;
    int destroyed = 0;
    auto bp = make_blueprint<request, E>() | await_io_uring_fast<operation, E>(&executor) | end();
    {
        io_uring_context ctx(ring.ring, 0xff17, 1);
        auto runner = make_fast_runner_view(bp, receiver{&seen});
        runner(make_request(ctx, destroyed));
        check(ctx.complete(ring.last_token(), 0, 0) && executor.pending && !seen.calls && !destroyed,
              "fast completion transfers owned buffer into deferred continuation");
        check(ctx.try_detach(), "backend attachment may detach before selected executor resumes");
    }
    executor.pending();
    executor.pending = task_wrapper_sbo{};
    check(seen.calls == 1 && seen.values == 1 && destroyed == 1,
          "deferred fast continuation completes after attachment destruction");
    auto runner = make_fast_runner_view(bp, receiver{&seen});
    runner(request{nullptr, operation{std::unique_ptr<payload>(new payload{&destroyed, 0}), -2}});
    check(executor.pending && destroyed == 2, "null fast attachment fails without leaking owned buffer");
    executor.pending();
    executor.pending = task_wrapper_sbo{};
    check(seen.calls == 2 && seen.values == 1, "null attachment delivers one error on selected executor");
}

void reentrant_case() {
    staged_ring ring;
    io_uring_context ctx(ring.ring, 0xff12, 1);
    struct nop { void prepare(io_uring_sqe* sqe) noexcept { io_uring_prep_nop(sqe); } } op;
    struct state { io_uring_context* ctx; nop* op; int calls = 0; } s{&ctx, &op, 0};
    auto callback = [](void* arg, int, unsigned) noexcept {
        auto* p = static_cast<state*>(arg);
        ++p->calls;
        check(p->ctx->enqueue(*p->op, [](void* arg2, int, unsigned) noexcept {
            ++static_cast<state*>(arg2)->calls;
        }, p) == 0, "completion may submit into just-freed slot");
    };
    check(ctx.enqueue(op, callback, &s) == 0, "initial callback registered");
    check(ctx.complete(ring.last_token(), 0, 0), "first reentrant completion");
    check(ctx.complete(ring.last_token(), 0, 0) && s.calls == 2 && !ctx.pending(), "reentrant completion drains");
}

void close_during_prepare() {
    staged_ring ring;
    io_uring_context ctx(ring.ring, 0xff14, 1);
    struct prepare_barrier {
        std::atomic<bool> entered{false}, release{false};
    } barrier;
    struct paused_operation {
        prepare_barrier* barrier;
        void prepare(io_uring_sqe* sqe) noexcept {
            barrier->entered.store(true, std::memory_order_release);
            while (!barrier->release.load(std::memory_order_acquire)) std::this_thread::yield();
            io_uring_prep_nop(sqe);
        }
    } op{&barrier};
    bool closer_ok = false;
    int calls = 0;
    std::thread closer([&] {
        while (!barrier.entered.load(std::memory_order_acquire)) std::this_thread::yield();
        ctx.close(); ctx.close();
        closer_ok = !ctx.closed() && ctx.pending() == 1;
        barrier.release.store(true, std::memory_order_release);
    });
    check(ctx.enqueue(op, [](void* p, int, unsigned) noexcept { ++*static_cast<int*>(p); }, &calls) == 0,
          "admitted SQ preparation survives concurrent close");
    closer.join();
    check(closer_ok && !ctx.try_detach(), "close observes in-progress preparation ticket");
    check(ctx.enqueue(op, [](void*, int, unsigned) noexcept {}, nullptr) == -ESHUTDOWN,
          "close rejects subsequent SQ preparation");
    check(ctx.complete(ring.last_token(), 0, 0) && calls == 1 && ctx.closed() && ctx.try_detach(),
          "terminal CQE returns the final close ticket");
}

template <typename Awaitable = io_uring_awaitable<operation, E>>
int kernel_case() {
    io_uring ring{};
    const int rc = io_uring_queue_init(8, &ring, 0);
    if (rc == -EPERM || rc == -ENOSYS || rc == -EACCES) {
        std::printf("[SKIP] io_uring unavailable: %d\n", rc);
        return 77;
    }
    if (rc) { std::printf("[FAIL] queue init: %d\n", rc); return 1; }
    {
        io_uring_context ctx(ring, 0xff13, 4);
        int destroyed = 0;
        observed seen;
        auto bp = make_blueprint<request, E>() | await<Awaitable>() | end();
        auto runner = make_runner(make_lite_ptr<decltype(bp)>(std::move(bp)), receiver{&seen});
        // A real file read exercises buffer retention and actual data transfer.
        FILE* file = std::tmpfile();
        if (!file) std::abort();
        std::fputc('F', file);
        std::fflush(file);
        runner(make_request(ctx, destroyed, fileno(file)));
        io_uring_sqe* foreign = io_uring_get_sqe(&ring);
        if (!foreign) std::abort();
        io_uring_prep_nop(foreign);
        io_uring_sqe_set_data64(foreign, 7);
        check(io_uring_submit(&ring) == 2, "host submits FF read and unrelated NOP together");
        int host_completed = 0;
        for (int i = 0; i < 2; ++i) {
            io_uring_cqe* cqe = nullptr;
            if (io_uring_wait_cqe(&ring, &cqe)) std::abort();
            const auto copy = *cqe;
            io_uring_cqe_seen(&ring, cqe);
            if (!ctx.complete(copy.user_data, copy.res, copy.flags))
                host_completed += copy.user_data == 7 && copy.res == 0;
        }
        check(seen.calls == 1 && seen.result == 1 && seen.byte == 'F' && destroyed == 1,
              "real read completes with owned buffer");
        check(host_completed == 1 && ctx.try_detach(), "shared ring preserves foreign completion and detaches");
        std::fclose(file);
    }
    io_uring_queue_exit(&ring);
    return failures ? 1 : 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--kernel") == 0) {
        const int ordinary = kernel_case();
        if (ordinary) return ordinary;
        return kernel_case<io_uring_fast_awaitable<operation, E>>();
    }
    flow_case(false, 42, false);
    flow_case(false, -EBADF, false);
    flow_case(true, 0, false);
    flow_case(false, 42, true);
    admission_case();
    flow_case<io_uring_fast_awaitable<operation, E>>(false, 42, true);
    flow_case<io_uring_fast_awaitable<operation, E>>(false, -EBADF, true);
    flow_case<io_uring_fast_awaitable<operation, E>>(false, 42, false);
    admission_case<io_uring_fast_awaitable<operation, E>>();
    terminal_cancel_waits_for_cqe();
    terminal_reentrant_and_lifetime();
    terminal_deferred_receiver();
    reentrant_case();
    close_during_prepare();
    return failures ? 1 : 0;
}
