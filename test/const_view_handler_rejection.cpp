#include "flow/flow.h"
#include <system_error>

namespace ff = flux_foundry;
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
using error_type = std::exception_ptr;
#else
using error_type = std::error_code;
#endif
using result = ff::result_t<int, error_type>;

struct Handler {
    result operator()(result&& r) & noexcept { return std::move(r); }
#if defined(FF_TEST_DIFFERENT_RESULT)
    ff::result_t<long, error_type> operator()(result&&) const & noexcept;
#elif defined(FF_TEST_THROWING_CONST)
    result operator()(result&&) const &;
#endif
};

constexpr auto bp = ff::make_blueprint<int, error_type>()
#if defined(FF_TEST_THEN)
    | ff::then(Handler{})
#elif defined(FF_TEST_ON_ERROR)
    | ff::on_error(Handler{})
#else
    | ff::on_error([count = 0](result&&) mutable noexcept { return result(ff::value_tag, ++count); })
#endif
    | ff::end();

int main() {
    (void)ff::make_fast_runner_view(bp);
}
