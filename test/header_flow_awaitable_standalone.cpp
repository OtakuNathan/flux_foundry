#include "flow/flow_awaitable.h"

namespace {
namespace ff = flux_foundry;

struct operation final : ff::fast_awaitable_base<operation, int, std::exception_ptr> {
    using async_result_type = ff::result_t<int, std::exception_ptr>;
    explicit operation(int) noexcept {}
    explicit operation(double) noexcept(false) {}
    bool available() const noexcept { return true; }
    int submit() noexcept { return 0; }
};

template <typename Arg>
auto accepts(int) -> decltype(ff::awaitable_factory<operation>{}(std::declval<Arg>()), std::true_type{});
template <typename Arg>
auto accepts(...) -> std::false_type;

static_assert(decltype(accepts<int>(0))::value, "accept the nonthrowing constructor");
static_assert(!decltype(accepts<double>(0))::value, "honor the explicitly disabled exception policy");
}

int main() {}
