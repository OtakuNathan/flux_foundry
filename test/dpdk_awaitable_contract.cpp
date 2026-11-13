#include "lifecycle_test_support.h"
#include "extension/dpdk_awaitable.h"
#include <cstdio>
#include <memory>

using namespace flux_foundry;
using namespace flux_foundry::extension;
using E = lifecycle_test::error;
namespace {
int failures = 0;
void check(bool ok, const char* message) {
    std::printf("[%s] %s\n", ok ? "OK" : "FAIL", message);
    if (!ok) ++failures;
}
struct fake_api {
    struct packet_type { int id; };
    static int ready, freed, accepted, rx_calls, allocated, tx_limit;
    static flow_controller* cancel_during_rx;
    static void free(packet_type* p) noexcept { ++freed; delete p; }
    static std::uint16_t rx(std::uint16_t port, std::uint16_t queue, packet_type** packets,
                            std::uint16_t capacity) noexcept {
        check(port == 3 && queue == 2, "RX uses borrowed queue identifiers");
        ++rx_calls;
        std::uint16_t n = 0;
        while (ready && n < capacity) {
            packets[n++] = new packet_type{++allocated};
            --ready;
        }
        if (cancel_during_rx) cancel_during_rx->cancel();
        return n;
    }
    static std::uint16_t tx(std::uint16_t, std::uint16_t, packet_type** packets,
                            std::uint16_t count) noexcept {
        std::uint16_t n = 0;
        while (n < count && n < tx_limit) { delete packets[n++]; ++accepted; }
        return n;
    }
    static void reset() { ready = freed = accepted = rx_calls = allocated = tx_limit = 0; cancel_during_rx = nullptr; }
};
int fake_api::ready = 0;
int fake_api::freed = 0;
int fake_api::accepted = 0;
int fake_api::rx_calls = 0;
int fake_api::allocated = 0;
int fake_api::tx_limit = 0;
flow_controller* fake_api::cancel_during_rx = nullptr;
using context = dpdk_rx_context<8, fake_api>;
using batch = context::batch_type;
using aw = dpdk_rx_awaitable<8, E, fake_api>;
using output = aw::async_result_type;
struct seen { int calls = 0, errors = 0; batch packets; };
struct receiver {
    using value_type = output;
    seen* result;
    void emplace(output&& out) noexcept {
        ++result->calls;
        if (out.has_error()) ++result->errors;
        else result->packets = std::move(out).value();
    }
};

template <typename Awaitable = aw>
void receive_case(bool fast) {
    fake_api::reset();
    context ctx(3, 2);
    seen result;
    auto bp = make_blueprint<context*, E>() | await<Awaitable>() | end();
    auto drive = [&](auto& runner) {
        runner(&ctx);
        check(ctx.pending() && !ctx.poll() && !result.calls, "empty RX keeps awaitable pending");
        fake_api::ready = 3;
        check(ctx.poll() && !ctx.pending() && result.calls == 1 && result.packets.size() == 3,
              "RX burst moves mbuf ownership into flow");
        check(!fake_api::freed && !ctx.poll(), "delivered mbufs stay owned; no waiter means no RX poll");
        fake_api::tx_limit = 0;
        check(result.packets.transmit(3, 2) == 0 && result.packets.size() == 3, "zero TX acceptance retains entire burst");
        fake_api::tx_limit = 1;
        check(result.packets.transmit(3, 2) == 1 && result.packets.size() == 2 && result.packets[0]->id == 2,
              "partial TX transfers only accepted prefix");
        batch moved(std::move(result.packets));
        check(result.packets.size() == 0 && moved.size() == 2, "moving batch transfers ownership");
        moved.clear();
        check(fake_api::freed == 2 && fake_api::accepted == 1, "unsent tail freed exactly once");
        check(ctx.try_detach(), "drained RX attachment detaches");
    };
    if (fast) {
        auto runner = make_fast_runner(make_lite_ptr<decltype(bp)>(std::move(bp)), receiver{&result});
        drive(runner);
    } else {
        auto runner = make_runner(make_lite_ptr<decltype(bp)>(std::move(bp)), receiver{&result});
        drive(runner);
    }
}

void cancel_case(bool during_rx, bool close_only) {
    fake_api::reset();
    context ctx(3, 2);
    seen result;
    auto control = make_lite_ptr<flow_controller>();
    auto bp = make_blueprint<context*, E>() | await<aw>() | end();
    flow_runner<decltype(bp), receiver> runner(make_lite_ptr<decltype(bp)>(std::move(bp)), control, receiver{&result});
    runner(&ctx);
    if (during_rx) {
        fake_api::ready = 3;
        fake_api::cancel_during_rx = control.get();
    } else if (close_only) {
        check(!ctx.try_detach(), "detach refuses pending RX waiter");
    } else {
        control->cancel();
        check(result.calls == 1 && result.errors == 1 && ctx.pending(), "logical cancel keeps backend waiter alive");
    }
    check(ctx.poll() && !ctx.pending() && result.calls == 1 && result.errors == 1, "poll drains cancellation exactly once");
    check(fake_api::freed == (during_rx ? 3 : 0), "packets arriving during cancellation are freed");
    check(fake_api::rx_calls == (during_rx ? 1 : 0), "idle cancellation and close require no NIC traffic");
}

template <typename Awaitable = aw>
void admission_case() {
    fake_api::reset();
    context ctx(3, 2);
    seen first, second;
    auto bp = make_blueprint<context*, E>() | await<Awaitable>() | end();
    auto shared = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto one = make_runner(shared, receiver{&first});
    auto two = make_runner(shared, receiver{&second});
    one(&ctx);
    two(&ctx);
    check(second.calls == 1 && second.errors == 1 && ctx.pending(), "second waiter is rejected without replacing first");
    ctx.close();
    check(ctx.poll() && first.calls == 1 && first.errors == 1, "close drains first waiter");
    auto three = make_runner(shared, receiver{&second});
    three(&ctx);
    check(second.calls == 2 && second.errors == 2 && ctx.closed(), "closed attachment rejects admission");
}

using fast_aw = dpdk_rx_fast_awaitable<8, E, fake_api>;

void terminal_cancel_and_close() {
    fake_api::reset();
    context ctx(3, 2);
    seen result;
    auto control = make_lite_ptr<flow_controller>();
    auto bp = make_blueprint<context*, E>() | await<fast_aw>() | end();
    flow_runner<decltype(bp), receiver> runner(make_lite_ptr<decltype(bp)>(std::move(bp)), control, receiver{&result});
    runner(&ctx);
    control->cancel();
    check(ctx.pending() && !result.calls, "fast RX has no early controller cancellation");
    ctx.close();
    check(ctx.poll() && ctx.closed() && result.calls == 1 && result.errors == 1,
          "fast RX close supplies the terminal outcome without traffic");
    check(fake_api::rx_calls == 0 && !ctx.poll(), "fast RX terminal close does not poll NIC or complete twice");
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
    fake_api::reset();
    std::unique_ptr<context> ctx(new context(3, 2));
    seen first, second;
    auto bp = make_blueprint<context*, E>() | await<fast_aw>() | end();
    auto initial_receiver = make_callback_receiver([&](output&& out) noexcept {
        check(out.has_value() && !ctx->pending(), "fast RX clears waiter before entering continuation");
        receiver{&first}.emplace(std::move(out));
        auto next = make_fast_runner_view(bp, receiver{&second});
        next(ctx.get());
        fake_api::ready = 2;
    });
    auto runner = make_fast_runner_view(bp, initial_receiver);
    runner(ctx.get());
    check(!ctx->poll() && !first.calls, "empty fast RX remains pending");
    fake_api::ready = 3;
    check(ctx->poll() && first.calls == 1 && first.packets.size() == 3 && ctx->pending(),
          "fast continuation may rearm the same RX attachment");
    check(ctx->poll() && second.calls == 1 && second.packets.size() == 2 && !ctx->pending(),
          "reentrant fast RX request completes once");
    first.packets.clear(); second.packets.clear();
    check(fake_api::freed == 5, "reentrant fast RX returns all owned packets");

    auto final_receiver = make_callback_receiver([&](output&& out) noexcept {
        receiver{&first}.emplace(std::move(out));
        check(!ctx->pending(), "fast result may destroy drained RX attachment");
        ctx.reset();
    });
    auto last = make_fast_runner_view(bp, final_receiver);
    last(ctx.get());
    fake_api::ready = 1;
    auto* attachment = ctx.get();
    check(attachment->poll() && !ctx && first.packets.size() == 1 && fake_api::freed == 5,
          "packet ownership survives attachment destruction in fast callback");
    first.packets.clear();
    check(fake_api::freed == fake_api::allocated, "destroying fast result frees final packet exactly once");
}

void terminal_deferred_receiver() {
    fake_api::reset();
    struct queued_executor {
        task_wrapper_sbo pending;
        void dispatch(task_wrapper_sbo&& task) noexcept { pending = std::move(task); }
    } executor;
    seen result;
    auto bp = make_blueprint<context*, E>() | await<fast_aw>(&executor) | end();
    {
        context ctx(3, 2);
        auto runner = make_fast_runner_view(bp, receiver{&result});
        runner(&ctx);
        fake_api::ready = 2;
        check(ctx.poll() && executor.pending && !result.calls && !fake_api::freed,
              "fast RX transfers packets into selected executor continuation");
        check(ctx.try_detach(), "RX attachment detaches before deferred continuation runs");
    }
    executor.pending(); executor.pending = task_wrapper_sbo{};
    check(result.calls == 1 && result.packets.size() == 2,
          "deferred fast RX result survives backend attachment destruction");
    result.packets.clear();
    // Retain deferred null-context coverage with an owning blueprint. GCC 14
    // otherwise warns about the inactive packet alternative when moving a
    // known error into a borrowed runner's temporary task closure. The guarded
    // union move is valid; borrowed deferred success is checked above, and
    // borrowed null-context rejection is checked separately below.
    auto runner = make_fast_runner(make_lite_ptr<decltype(bp)>(std::move(bp)), receiver{&result});
    runner(static_cast<context*>(nullptr));
    check(bool(executor.pending), "null fast RX attachment routes error to selected executor");
    executor.pending(); executor.pending = task_wrapper_sbo{};
    check(result.calls == 2 && result.errors == 1 && fake_api::freed == fake_api::allocated,
          "null fast RX attachment produces one error without leaked packets");
}

void terminal_borrowed_null_receiver() {
    fake_api::reset();
    seen result;
    auto bp = make_blueprint<context*, E>() | await<fast_aw>() | end();
    auto runner = make_fast_runner_view(bp, receiver{&result});
    runner(static_cast<context*>(nullptr));
    runner(static_cast<context*>(nullptr));
    check(result.calls == 2 && result.errors == 2 && !result.packets.size()
              && !fake_api::rx_calls && !fake_api::allocated,
          "borrowed fast RX null-context rejection stays synchronous and reusable");
}

void batch_move_cases() {
    for (const int count : {0, 1, 3, 8}) {
        fake_api::reset();
        fake_api::ready = count;
        auto source = batch::receive(3, 2);
        fake_api::ready = 2;
        auto destination = batch::receive(3, 2);
        destination = std::move(source);
        check(source.size() == 0 && destination.size() == count && fake_api::freed == 2,
              "move assignment releases old ownership and transfers empty/partial/full burst");
        for (int i = 0; i < count; ++i)
            check(destination[static_cast<std::uint16_t>(i)]->id == i + 1,
                  "moved burst preserves valid packet order");
        auto* alias = &destination;
        destination = std::move(*alias);
        check(destination.size() == count && fake_api::freed == 2, "self-move preserves packet ownership");
        batch moved(std::move(destination));
        check(destination.size() == 0 && moved.size() == count, "move construction transfers active prefix");
        fake_api::tx_limit = 1;
        const auto sent = moved.transmit(3, 2);
        batch tail(std::move(moved));
        check(moved.size() == 0 && tail.size() == count - sent,
              "move after partial TX retains only the unsent packets");
        tail.clear();
        source.clear();
        destination.clear();
        moved.clear();
        check(fake_api::freed + fake_api::accepted == fake_api::allocated,
              "empty/partial/full move chains release each packet exactly once");
    }
}
} // namespace
int main() {
    receive_case(false);
    receive_case(true);
    cancel_case(false, false);
    cancel_case(true, false);
    cancel_case(false, true);
    admission_case();
    receive_case<fast_aw>(false);
    receive_case<fast_aw>(true);
    admission_case<fast_aw>();
    terminal_cancel_and_close();
    terminal_reentrant_and_lifetime();
    terminal_deferred_receiver();
    terminal_borrowed_null_receiver();
    batch_move_cases();
    return failures ? 1 : 0;
}
