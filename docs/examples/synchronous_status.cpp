#include "flow/flow.h"

#include <cstdio>
#include <utility>

namespace ff = flux_foundry;

enum class status { invalid_input };
using result = ff::result_t<int, status>;

struct receiver {
    using value_type = result;
    int* successes;
    int* errors;

    void emplace(result&& output) noexcept {
        if (output.has_value()) {
            ++*successes;
            std::printf("value=%d\n", output.value());
        } else {
            ++*errors;
            std::printf("error=invalid_input\n");
        }
    }
};

int main() {
    int successes = 0;
    int errors = 0;
    auto blueprint = ff::make_blueprint<int, status>()
        | ff::then([](result&& input) noexcept -> result {
            const int value = input.value(); // then invokes us only on success
            if (value < 0 || value > 100) {
                return result(ff::error_tag, status::invalid_input);
            }
            return result(ff::value_tag, value);
        })
        | ff::transform([](int value) noexcept { return value * 3 + 1; })
        | ff::end();

    // The view borrows blueprint. All calls finish before it leaves scope.
    for (int value : {2, -1, 3}) {
        auto runner = ff::make_fast_runner_view(
            blueprint, receiver{&successes, &errors});
        runner(value);
    }
    return successes == 2 && errors == 1 ? 0 : 1;
}
