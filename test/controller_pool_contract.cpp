#include "lifecycle_test_support.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace flux_foundry;

namespace {
using error = lifecycle_test::error;
using output = result_t<int, error>;
struct receiver {
    using value_type = output;
    int* value;
    void emplace(output&& result) noexcept { *value = result.has_value() ? result.value() : -1; }
};
struct counts { std::atomic<int> allocated{0}, freed{0}; };
struct counting_allocator {
    counts* count;
    void* allocate(size_t alignment, size_t bytes) noexcept {
        auto* p = flux_foundry::aligned_alloc(alignment, bytes);
        if (p) count->allocated.fetch_add(1, std::memory_order_relaxed);
        return p;
    }
    void deallocate(void* p) const noexcept {
        if (p) count->freed.fetch_add(1, std::memory_order_relaxed);
        aligned_free(p);
    }
};

struct failing_allocator {
    int* attempts;
    void* allocate(size_t, size_t) noexcept { ++*attempts; return nullptr; }
    void deallocate(void*) const noexcept { std::abort(); }
};

bool allocation_failure() {
    auto bp = make_blueprint<int, error>() | transform([](int x) noexcept { return x + 1; }) | end();
    auto shared = make_lite_ptr<decltype(bp)>(std::move(bp));
    int attempts = 0, value = 99;
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    try {
        auto runner = make_runner(std::allocator_arg, failing_allocator{&attempts}, shared, receiver{&value});
        (void)runner;
        return false;
    } catch (const std::bad_alloc&) {
        return attempts == 1 && value == 99;
    }
#else
    auto runner = make_runner(std::allocator_arg, failing_allocator{&attempts}, shared, receiver{&value});
    if (runner.get_controller()) return false;
    runner(41);
    return attempts == 1 && value == 99;
#endif
}

bool retained_controller() {
    counts count;
    auto bp = make_blueprint<int, error>() | transform([](int x) noexcept { return x + 1; }) | end();
    auto shared = make_lite_ptr<decltype(bp)>(std::move(bp));
    using pointer = decltype(make_lite_ptr_with_allocator<flow_controller>(counting_allocator{&count}));
    pointer old;
    int first = 0, second = 0;
    {
        auto runner = make_runner(std::allocator_arg, counting_allocator{&count}, shared, receiver{&first});
        runner(41);
        old = runner.get_controller();
    }
    if (first != 42 || count.allocated != 1 || count.freed != 0) return false;
    {
        auto next = make_runner(std::allocator_arg, counting_allocator{&count}, shared, receiver{&second});
        if (old.get() == next.get_controller().get()) return false;
        // A late cancellation handle must still name the old operation.
        old->cancel(true);
        next(8);
        if (second != 9 || next.get_controller()->is_canceled()) return false;
    }
    if (count.allocated != 2 || count.freed != 1) return false;
    std::thread release([handle = std::move(old)]() mutable { handle.release(); });
    release.join();
    return count.freed == 2;
}

bool pooled_alignment_and_reuse() {
    using allocator = pooled_controller_allocator<32, 1>;
    auto first = make_lite_ptr_with_allocator<flow_controller>(allocator{});
    if (!first || reinterpret_cast<std::uintptr_t>(first.get()) % alignof(flow_controller)) return false;
    auto* address = first.get();
    first->cancel(true);
    first.release();
    auto second = make_lite_ptr_with_allocator<flow_controller>(allocator{});
    if (!second || second.get() != address || second->is_canceled()) return false;
    auto held = second;
    second.release();
    auto third = make_lite_ptr_with_allocator<flow_controller>(allocator{});
    if (!third || third.get() == held.get()) return false;
    return true;
}

bool remote_return_after_owner_exit() {
    using allocator = pooled_controller_allocator<48, 1>;
    using pointer = decltype(make_lite_ptr_with_allocator<flow_controller>(allocator{}));
    pointer controller;
    std::thread owner([&] { controller = make_lite_ptr_with_allocator<flow_controller>(allocator{}); });
    owner.join();
    if (!controller) return false;
    controller->cancel();
    controller.release();
    allocator::flush_remote_returns(); // Stale origin is adopted, never dereferenced.
    auto next = make_lite_ptr_with_allocator<flow_controller>(allocator{});
    return next && !next->is_canceled();
}

bool pooled_runner() {
    auto bp = make_blueprint<int, error>() | transform([](int x) noexcept { return x * 2; }) | end();
    auto shared = make_lite_ptr<decltype(bp)>(std::move(bp));
    int value = 0;
    auto runner = make_runner(std::allocator_arg, pooled_controller_allocator<>{}, shared, receiver{&value});
    runner(21);
    if (value != 42 || runner.is_consumed()) return false;
    runner(8);
    auto controller = runner.get_controller();
    controller->cancel(true);
    runner(99);
    auto ignored = make_runner(std::allocator_arg, pooled_controller_allocator<>{}, shared);
    ignored(1);
    return value == -1 && controller->is_force_canceled();
}

struct delayed;
struct request { delayed** slot; int* destroyed; };
struct delayed final : awaitable_base<delayed, int, error> {
    using async_result_type = output;
    request input;
    explicit delayed(result_t<request, error>&& in) noexcept : input(in.value()) {}
    ~delayed() { ++*input.destroyed; }
    bool available() const noexcept { return true; }
    int submit() noexcept { this->retain(); *input.slot = this; return 0; }
    void cancel() noexcept {}
    void complete() noexcept {
        this->resume(output(value_tag, 42));
        this->release(); // Backend retain protects a late terminal completion.
    }
};

bool cancellation_before_backend_completion() {
    using allocator = pooled_controller_allocator<64, 1>;
    using pointer = decltype(make_lite_ptr_with_allocator<flow_controller>(allocator{}));
    pointer control;
    delayed* pending = nullptr;
    int value = 0, destroyed = 0;
    {
        auto bp = make_blueprint<request, error>() | await<delayed>() | end();
        auto runner = make_runner(std::allocator_arg, allocator{},
            make_lite_ptr<decltype(bp)>(std::move(bp)), receiver{&value});
        runner(request{&pending, &destroyed});
        control = runner.get_controller();
        if (!runner.is_consumed() || !pending) return false;
    }
    std::thread cancel([&] { control->cancel(true); });
    cancel.join();
    if (value != -1 || destroyed != 0) return false;
    auto next = make_lite_ptr_with_allocator<flow_controller>(allocator{});
    if (next.get() == control.get() || next->is_canceled()) return false;
    pending->complete();
    return value == -1 && destroyed == 1 && control->is_force_canceled();
}
}

int main() {
    bool ok = true;
    auto check = [&ok](const char* name, bool passed) {
        std::printf("[%s] %s\n", passed ? "OK" : "FAIL", name);
        ok = passed && ok;
    };
    check("last external owner controls reclamation; stale cancel stays isolated", retained_controller());
    check("explicit allocator failure preserves allocation policy", allocation_failure());
    check("pooled control block alignment and fresh state on reuse", pooled_alignment_and_reuse());
    check("return after allocating thread exits", remote_return_after_owner_exit());
    check("normal runner with explicit pooled controller", pooled_runner());
    check("pooled controller cancellation before late backend completion", cancellation_before_backend_completion());
    return ok ? 0 : 1;
}
