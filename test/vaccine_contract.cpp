#include "lifecycle_test_support.h"
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace flux_foundry;
namespace {
using error = lifecycle_test::error;
using output = result_t<int, error>;
struct executor {
    std::vector<task_wrapper_sbo> tasks;
    void dispatch(task_wrapper_sbo&& t) noexcept { tasks.emplace_back(std::move(t)); }
    void drain() { auto pending = std::move(tasks); for (auto& t : pending) t(); }
};
struct receiver {
    using value_type = output;
    int* result;
    error* failure;
    void emplace(output&& out) noexcept {
        if (out.has_value()) *result = out.value();
        else *failure = std::move(out.error());
    }
};
struct add_one { int operator()(int x) const noexcept { return x + 1; } };
struct check_positive {
    int* calls;
    bool operator()(const output& out) const noexcept { ++*calls; return out.has_error() || out.value() > 0; }
};
struct constant_check {
    bool operator()(const output& out) const noexcept { return out.has_value(); }
};
constexpr auto constant_bp = make_blueprint<int, error>() | position("constant") | vaccine(constant_check{});
static_assert(decltype(constant_bp)::node_count == (FLUX_FOUNDRY_ENABLE_VACCINE ? 3 : 1),
              "C++14 constant construction must preserve/erase checkpoints according to policy");
auto plain() { return make_blueprint<int, error>() | transform(add_one{}) | end(); }
auto marked(int* calls) {
    return make_blueprint<int, error>() | position("input") | vaccine(check_positive{calls})
        | position("increment", transform(add_one{})) | vaccine("positive", check_positive{calls}) | end();
}
bool forwarding() {
    (void)constant_bp;
    int calls = 0, result = 0;
    error failure;
    auto bp = marked(&calls);
    auto reference = plain();
    (void)reference;
#if !FLUX_FOUNDRY_ENABLE_VACCINE
    static_assert(std::is_same<decltype(bp), decltype(plain())>::value,
                  "disabled markers must disappear before fusion");
    static_assert(sizeof(bp) == sizeof(plain()), "disabled markers must not grow the blueprint");
#endif
    auto ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto normal = make_runner(ptr, receiver{&result, &failure});
    normal(41);
    bool ok = result == 42;
    auto fast = make_fast_runner(ptr, receiver{&result, &failure});
    fast(9);
    ok = ok && result == 10;
    const auto const_bp = marked(&calls);
    auto view = make_fast_runner_view(const_bp, receiver{&result, &failure});
    view(6);
    ok = ok && result == 7;
    const auto sent = lifecycle_test::failure();
    view(output(error_tag, sent));
    ok = ok && failure == sent;
    return ok && calls == (FLUX_FOUNDRY_ENABLE_VACCINE ? 8 : 0);
}
bool queued_and_move_only() {
    using owned_output = result_t<std::unique_ptr<int>, error>;
    executor queue;
    int checks = 0, result = 0;
    auto bp = make_blueprint<std::unique_ptr<int>, error>()
        | position("queued", via(&queue))
        | vaccine("owns payload", [&checks](const owned_output& out) noexcept {
            ++checks; return out.has_value() && out.value() && *out.value() == 42;
        }) | transform([](std::unique_ptr<int> p) noexcept { return *p; }) | end();
    error failure;
    {
        auto runner = make_fast_runner(std::move(bp), receiver{&result, &failure});
        runner(std::unique_ptr<int>(new int(42)));
    }
    if (checks || result) return false;
    queue.drain();
    return result == 42 && checks == (FLUX_FOUNDRY_ENABLE_VACCINE ? 1 : 0);
}
bool void_result() {
    int checks = 0, delivered = 0;
    auto bp = make_blueprint<int, error>() | transform([](int) noexcept {}) | position("void result")
        | vaccine([&checks](const result_t<void, error>& out) noexcept { ++checks; return out.has_value(); })
        | transform([&delivered]() noexcept { ++delivered; }) | end();
    auto runner = make_fast_runner(std::move(bp));
    runner(1);
    return delivered == 1 && checks == (FLUX_FOUNDRY_ENABLE_VACCINE ? 1 : 0);
}
void reporting_hook(const contract_violation& failure) noexcept {
    const bool correct = failure.position.name && std::strcmp(failure.position.name, "selected") == 0 &&
        failure.position.line == 123 && failure.position.file && std::strcmp(failure.position.file, "user_pipeline.cpp") == 0;
    std::fprintf(stderr, "HOOK %s reason=%d contract=%s\n", correct ? "OK" : "FAIL", int(failure.reason), failure.contract);
    std::fflush(stderr);
    // Returning must not permit the pipeline to continue.
}
void failing_pipeline(bool throwing, bool custom) {
    auto bp = make_blueprint<int, error>() | position("ignored") | transform(add_one{})
        | position("selected", "user_pipeline.cpp", 123)
        | vaccine("expected failure", [throwing](const output&) noexcept(!FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS) {
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            if (throwing) throw std::runtime_error("predicate failure");
#else
            (void)throwing;
#endif
            return false;
        }, custom ? reporting_hook : nullptr)
        | transform([](int n) noexcept { std::fputs("UNREACHABLE\n", stderr); return n; }) | end();
    auto runner = make_fast_runner(std::move(bp));
    runner(1);
}
} // namespace
int main(int argc, char** argv) {
    if (argc > 1) {
        // Turn the expected abort into a portable subprocess status (no core/UI).
        if (std::signal(SIGABRT, [](int) { std::_Exit(86); }) == SIG_ERR) return 2;
        failing_pipeline(std::strcmp(argv[1], "throw") == 0, std::strcmp(argv[1], "default") != 0);
        return 0;
    }
    bool ok = forwarding();
    ok = queued_and_move_only() && ok;
    ok = void_result() && ok;
    std::printf("[%s] vaccine enabled=%d: normal/fast/const-view, error, queued move-only and void\n",
        ok ? "OK" : "FAIL", FLUX_FOUNDRY_ENABLE_VACCINE);
    return ok ? 0 : 1;
}
