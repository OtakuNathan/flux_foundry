#include <flow/flow.h>

namespace {

using result_type = flux_foundry::result_t<int, std::exception_ptr>;

struct receiver {
    using value_type = result_type;
    int* output;

    void emplace(value_type&& result) noexcept {
        *output = result.has_value() ? result.value() : -1;
    }
};

} // namespace

int main() {
    auto blueprint = flux_foundry::make_blueprint<int>()
        | flux_foundry::transform([](int value) noexcept { return value + 1; })
        | flux_foundry::end();
    auto blueprint_ptr = flux_foundry::make_lite_ptr<decltype(blueprint)>(std::move(blueprint));

    int output = 0;
    auto runner = flux_foundry::make_runner(std::move(blueprint_ptr), receiver{&output});
    runner(41);
    return output == 42 ? 0 : 1;
}
