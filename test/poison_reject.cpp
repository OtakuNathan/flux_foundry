#include "flow/flow_poison.h"
using namespace flux_foundry;
using result = result_t<int, std::exception_ptr>;
int main() {
#if FF_REJECT_CASE == 1
    auto select = [](result&) noexcept { return poison_token{}; };
#elif FF_REJECT_CASE == 2
    auto select = [](const result&) noexcept { return true; };
#else
    auto select = [](const result&) { return poison_token{}; };
#endif
    auto bp = make_blueprint<int>() | poison("bad", select);
    (void)bp;
}
