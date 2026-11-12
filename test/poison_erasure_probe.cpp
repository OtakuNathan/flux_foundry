#include "flow/flow.h"
using namespace flux_foundry;
struct add { int operator()(int x) const noexcept { return x + 1; } };
struct select_token {
    poison_token operator()(const result_t<int, std::exception_ptr>&) const noexcept { return {}; }
};
volatile int observed;
struct receiver { using value_type = result_t<int, std::exception_ptr>; void emplace(value_type&& r) noexcept { observed = r.value(); } };
int main(int argc, char**) {
    auto bp = make_blueprint<int>()
#if FF_WITH_MARKERS
        | poison("erase", select_token{})
#endif
        | transform(add{}) | end();
#if FF_WITH_MARKERS
    auto plain = make_blueprint<int>() | transform(add{}) | end();
    static_assert(std::is_same<decltype(bp), decltype(plain)>::value, "node must erase");
    poison_scope scope;
    auto ref = scope.borrow(argc);
    auto check = scope.submission();
    auto callback = FLUX_FOUNDRY_POISON_CALLBACK(check, [ref]() noexcept { observed = *ref; });
    callback(); check.submitted(0); check.quiesce();
#else
    auto ref = &argc;
    auto callback = [ref]() noexcept { observed = *ref; };
    callback();
#endif
    make_fast_runner(std::move(bp), receiver{})(argc);
}
