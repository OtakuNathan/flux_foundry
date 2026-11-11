// BP construction may be constant; requests and execution remain runtime operations.
#include "flow/flow.h"
#include <cstdio>
#include <stdexcept>
#include <system_error>
namespace ff = flux_foundry;
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
using error_type = std::exception_ptr;
#else
using error_type = std::error_code;
#endif
using output_type = ff::result_t<int, error_type>;
struct add_three {
    int operator()(int input) const noexcept { return input + 3; }
};
#if FLUX_FOUNDRY_CPP_VER >= 201703L
constexpr
#endif
auto build_demo_blueprint() {
    return ff::make_blueprint<int, error_type>() | ff::transform(add_three{}) | ff::end();
}

#if FLUX_FOUNDRY_CPP_VER >= 201703L
// Constructing wrappers is constant even when their call operators run at runtime.
constexpr bool check_constant_construction() {
    auto bp = ff::make_blueprint<int, error_type>()
        | ff::transform(add_three{}) | ff::transform(add_three{})
        | ff::transform(add_three{}) | ff::transform(add_three{})
        | ff::transform(add_three{}) | ff::transform(add_three{})
        | ff::transform(add_three{}) | ff::transform(add_three{})
        | ff::transform(add_three{}) | ff::transform(add_three{})
        | ff::then([](output_type&& r) noexcept { return std::move(r); })
        | ff::on_error([](output_type&& r) noexcept { return std::move(r); })
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        | ff::catch_exception<std::runtime_error>([](const std::runtime_error&) noexcept { return 0; })
#endif
        | ff::end([](output_type&& r) noexcept { return std::move(r); });
    (void)bp;
    auto void_bp = ff::make_blueprint<int, error_type>()
        | ff::transform([](int) noexcept {})
        | ff::transform([]() noexcept { return 1; }) | ff::end();
    (void)void_bp;
    return true;
}
static_assert(check_constant_construction(), "blueprint composition must support constant evaluation");
#endif

#if defined(__cpp_constinit) && __cpp_constinit >= 201907L
// Guaranteed constant initialization, NOT a const BP; no runner changes needed.
constinit auto reusable_bp = build_demo_blueprint();
#else
// Same runtime behavior on the C++14/17 baseline.
auto reusable_bp = build_demo_blueprint();
#endif
struct demo_receiver {
    using value_type = output_type;
    int* output;
    void emplace(value_type&& r) noexcept { *output = r.has_value() ? r.value() : -1; }
};
int main() {
    int output = 0;
    auto runner = ff::make_fast_runner_view(reusable_bp, demo_receiver{&output});
    runner(4); if (output != 7) return 1;
    runner(8); if (output != 11) return 2;
    std::puts("constant-capable blueprint, runtime input and reusable execution: OK");
}
