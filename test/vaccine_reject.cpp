#include "flow/flow.h"
using namespace flux_foundry;
using output = result_t<int, std::exception_ptr>;
struct awaitable final : fast_awaitable_base<awaitable, int, std::exception_ptr> {
    using async_result_type = output;
    explicit awaitable(output&&) noexcept {}
    int submit() noexcept { return -1; }
};
int main() {
#if FF_REJECT_CASE == 4
    auto bp = make_blueprint<int>() | await<awaitable>() | position("marker")
        | via(flow_impl::inline_executor::executor()) | end();
    (void)bp;
#else
#if FF_REJECT_CASE == 1
    auto check = [](output&) noexcept { return true; };
#elif FF_REJECT_CASE == 2
    auto check = [](const output&) noexcept { return 1; };
#else
    auto check = [](const output&) { return true; };
#endif
    auto bp = make_blueprint<int>() | vaccine(check) | end();
    (void)bp;
#endif
}
