#include "flow/flow.h"
#include <cstdio>
#include <exception>
#include <system_error>
namespace ff = flux_foundry;
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
using error_type = std::exception_ptr;
#else
using error_type = std::error_code;
#endif
using output = ff::result_t<int, error_type>;
struct receiver {
    using value_type = output;
    int* value;
    void emplace(output&& result) noexcept { *value = result.has_value() ? result.value() : -1; }
};
int main() {
    int then_calls = 0, end_calls = 0, reference_value = 0, value = 0;
    auto bp = ff::make_blueprint<int, error_type>()
        | ff::then([count = 0, &then_calls](output&&) mutable noexcept -> output {
            then_calls = ++count;
            return output(ff::error_tag, error_type{});
        })
        | ff::on_error([state = 40](output&&) mutable noexcept -> output {
            return output(ff::value_tag, ++state);
        })
        | ff::transform([&reference_value](int n) noexcept { reference_value = n; return n + 1; })
        | ff::end([count = 0, &end_calls](output&& result) mutable noexcept -> output {
            end_calls = ++count;
            return std::move(result);
        });
    auto owned = ff::make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = ff::make_fast_runner(owned, receiver{&value});
    runner(0);
    const bool valid = value == 42 && then_calls == 1 && end_calls == 1 && reference_value == 41;
    std::printf("[%s] mutable value capture and reference capture through then/on_error/end\n", valid ? "OK" : "FAIL");
    return valid ? 0 : 1;
}
