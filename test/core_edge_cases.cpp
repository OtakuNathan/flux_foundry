#include <cstdio>
#include <type_traits>
#include <utility>

#include "memory/result_t.h"
#include "utility/concurrent_queues.h"

using namespace flux_foundry;

namespace {

struct copy_fallback_error {
    int code;

    explicit copy_fallback_error(int value) noexcept : code(value) {}
    copy_fallback_error(const copy_fallback_error&) noexcept = default;
    copy_fallback_error(copy_fallback_error&& other) noexcept(false) : code(other.code) {}
};

static_assert(std::is_nothrow_copy_constructible<copy_fallback_error>::value,
              "the test error must provide the strong copy fallback");
static_assert(!std::is_nothrow_move_constructible<copy_fallback_error>::value,
              "the test must exercise the throwing-move emplace_second overload");

bool test_spsc_variadic_emplace() {
    spsc_queue<std::pair<int, int>, 4> queue;
    if (!queue.try_emplace(11, 31)) {
        return false;
    }

    auto value = queue.try_pop();
    return value.has_value() && value.get().first == 11 && value.get().second == 31;
}

bool test_result_error_copy_fallback() {
    result_t<int, copy_fallback_error> result(value_tag, 7);
    copy_fallback_error first_error{41};
    result.emplace_error(first_error);
    if (!result.has_error() || result.error().code != 41) {
        return false;
    }

    copy_fallback_error replacement{42};
    result.emplace_error(replacement);
    return result.has_error() && result.error().code == 42;
}

} // namespace

int main() {
    const bool spsc_ok = test_spsc_variadic_emplace();
    const bool result_ok = test_result_error_copy_fallback();

    std::printf("[%s] spsc variadic emplace\n", spsc_ok ? "OK" : "FAIL");
    std::printf("[%s] result_t error copy fallback\n", result_ok ? "OK" : "FAIL");
    return spsc_ok && result_ok ? 0 : 1;
}
