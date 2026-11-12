#include "lifecycle_test_support.h"
#include "executor/simple_executor.h"
#include <csignal>
#include <cstring>
#include <memory>
#include <thread>
using namespace flux_foundry;
using output = result_t<int, lifecycle_test::error>;
struct receiver { using value_type = output; int* out; void emplace(output&& r) noexcept { *out = r.has_value() ? r.value() : -1; } };
struct TS_EMPTY_BASES op final : fast_awaitable_base<op, int, lifecycle_test::error>, private poison_scope {
    using async_result_type = output;
    int value;
    explicit op(output&& r) noexcept : value(r.value()) {}
    ~op() { invalidate(); }
    bool available() const noexcept { return true; }
    int submit() noexcept {
        auto check = submission({"inline", __FILE__, __LINE__});
        auto cb = FLUX_FOUNDRY_POISON_CALLBACK(check, [this]() noexcept { resume(output(value_tag, value + 1)); });
        cb();
        return check.submitted(0);
    }
};
struct counted_callback {
    int* moves;
    explicit counted_callback(int* count) noexcept : moves(count) {}
    counted_callback(counted_callback&& other) noexcept : moves(other.moves) { ++*moves; }
    void operator()() const noexcept {}
};
void hook(const poison_violation& v) noexcept {
    std::fprintf(stderr, "POISON reason=%d\n", int(v.reason));
}
int main(int argc, char** argv) {
    if (argc > 1) {
        std::signal(SIGABRT, [](int) { std::_Exit(86); });
        poison_scope scope({"request", __FILE__, __LINE__}, hook);
        int value = 42;
        auto ref = scope.borrow(value);
        auto check = scope.submission({"completion", __FILE__, __LINE__});
        auto cb = check.wrap([]() noexcept {});
        const char* mode = argv[1];
        if (!std::strcmp(mode, "expired")) { scope.invalidate(); (void)*ref; }
        if (!std::strcmp(mode, "duplicate")) { check.submitted(0); cb(); cb(); }
        if (!std::strcmp(mode, "rejected")) { check.submitted(-1); cb(); }
        if (!std::strcmp(mode, "inline-rejected")) { cb(); check.submitted(-1); }
        if (!std::strcmp(mode, "missing")) { check.submitted(0); check.quiesce(); }
        if (!std::strcmp(mode, "late")) { check.submitted(0); scope.invalidate(); cb(); }
        if (!std::strcmp(mode, "race")) {
            check.submitted(0);
            auto copy = cb;
            std::thread a([&] { cb(); }), b([&] { copy(); }); a.join(); b.join();
        }
        if (!std::strcmp(mode, "reuse")) {
            simple_executor<4> queue;
            {
                poison_scope window({"request A", __FILE__, __LINE__}, hook);
                auto old = window.borrow(value);
                queue.dispatch(task_wrapper_sbo([old]() noexcept { (void)*old; }));
            }
            value = 99; // Storage remains valid; the logical request has changed.
            queue.try_shutdown(); queue.run();
        }
        if (!std::strcmp(mode, "destroyed")) {
            auto owner = std::unique_ptr<poison_scope>(new poison_scope({}, hook));
            auto stale = owner->submission();
            auto raw = owner.get();
            auto completion = stale.wrap([raw]() noexcept { raw->invalidate(); });
            stale.submitted(0); owner.reset(); completion();
        }
        if (!std::strcmp(mode, "node")) {
            auto bp = make_blueprint<poison_ref<int>, lifecycle_test::error>()
                | poison("last use", [](const auto& r) noexcept { return poison_token_of(r.value()); })
                | transform([](poison_ref<int> p) noexcept { return *p; }) | end();
            make_fast_runner(std::move(bp))(ref);
        }
        std::fputs("UNREACHABLE\n", stderr); return 1;
    }
    const int immutable = 7;
    poison_scope first, second;
    int a = 1, b = 2;
    auto ra = first.borrow(a), rb = second.borrow(b);
    if (*first.borrow(immutable) != 7) return 5;
    auto shared = make_blueprint<poison_ref<int>, lifecycle_test::error>()
        | poison("end borrow", [](const auto& r) noexcept {
            return r.has_value() ? poison_token_of(r.value()) : poison_token{};
        }) | end();
    auto view = make_fast_runner_view(shared);
    view(ra);
    if (*rb != 2) return 6;
    view(rb);
    int value = 41, observed = 0;
    poison_scope scope;
    auto ref = scope.borrow(value);
    if (*ref != 41) return 1;
    auto check = scope.submission();
    auto cb = check.wrap([p = std::unique_ptr<int>(new int(42)), &observed]() noexcept { observed = *p; });
    check.submitted(0);
    std::thread worker(std::move(cb)); worker.join(); check.quiesce();
    if (observed != 42) return 2;
    auto const_check = scope.submission();
    const auto const_callback = const_check.wrap([](int) noexcept {});
    const_callback(42); const_check.submitted(0); const_check.quiesce();
    int plain_moves = 0, checked_moves = 0;
    auto plain_callback = counted_callback(&plain_moves);
    auto move_check = scope.submission();
    auto checked_callback = FLUX_FOUNDRY_POISON_CALLBACK(move_check, counted_callback(&checked_moves));
    plain_callback(); checked_callback(); move_check.submitted(0); move_check.quiesce();
#if !FLUX_FOUNDRY_ENABLE_POISON
    if (plain_moves != checked_moves) return 7;
#endif
    auto rejected = scope.submission(); rejected.submitted(-1); rejected.quiesce();
    auto bp = make_blueprint<int, lifecycle_test::error>() | await<op>() | end();
    make_fast_runner(std::move(bp), receiver{&observed})(41);
    if (observed != 42) return 3;
    simple_executor<4> queue;
    auto borrowed = make_blueprint<poison_ref<int>, lifecycle_test::error>() | via(&queue)
        | transform([](poison_ref<int> p) noexcept { return *p + 1; }) | end();
    make_fast_runner(std::move(borrowed), receiver{&observed})(ref);
    queue.try_shutdown(); queue.run();
    if (observed != 42) return 4;
#if !FLUX_FOUNDRY_ENABLE_POISON
    static_assert(std::is_same<poison_ref<int>, int*>::value, "raw reference when disabled");
    static_assert(std::is_empty<poison_scope>::value, "empty scope when disabled");
    struct holder { void* data; };
    struct plain : holder { int value; };
    struct TS_EMPTY_BASES instrumented : holder, poison_scope { int value; };
    static_assert(sizeof(plain) == sizeof(instrumented), "owner layout must not grow");
    auto f = []() noexcept {};
    static_assert(std::is_same<decltype(f), decltype(check.wrap(f))>::value, "original callback when disabled");
#endif
    std::puts("poison contracts passed");
}
