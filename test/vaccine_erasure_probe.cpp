#include "flow/flow.h"
using namespace flux_foundry;
struct increment { int operator()(int v) const noexcept { return v + 1; } };
struct predicate {
    char padding[128]{};
    bool operator()(const result_t<int, std::exception_ptr>& r) const noexcept { return r.has_value(); }
};
volatile int observed = 0;
struct receiver {
    using value_type = result_t<int, std::exception_ptr>;
    void emplace(value_type&& r) noexcept { observed = r.has_value() ? r.value() : -1; }
};
int main(int argc, char**) {
    auto bp = make_blueprint<int>()
#if FF_WITH_MARKERS
        | position("before") | vaccine("valid", predicate{})
#endif
        | transform(increment{})
#if FF_WITH_MARKERS
        | position("after") | vaccine("valid", predicate{})
#endif
        | end();
    auto runner = make_runner(make_lite_ptr<decltype(bp)>(std::move(bp)), receiver{});
    runner(argc);
}
