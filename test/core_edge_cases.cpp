#include <array>
#include <cstdio>
#include <memory>
#include <type_traits>
#include <utility>

#include "memory/result_t.h"
#include "utility/concurrent_queues.h"
#include "utility/callable_wrapper.h"
#include "task/task_core.h"

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

bool test_large_byte_aligned_callable() {
    std::array<char, 2048> payload{};
    payload.front() = 17;
    payload.back() = 25;
    callable_wrapper<int()> callable([payload]() noexcept {
        return payload.front() + payload.back();
    });
    auto copy = callable;
    return callable() == 42 && copy() == 42;
}

struct member_task_target {
    int value = 42;
    int read() const noexcept { return value; }
};

using borrowed_member_task = task<decltype(&member_task_target::read), member_task_target>;
static_assert(!std::is_constructible<borrowed_member_task,
                  decltype(&member_task_target::read), member_task_target&&>::value,
              "temporary borrowed objects must remain rejected");
static_assert(std::is_constructible<borrowed_member_task,
                  decltype(&member_task_target::read), member_task_target&>::value,
              "lvalue borrowed objects must remain supported");

bool test_shared_member_task() {
    auto object = std::make_shared<member_task_target>();
    std::weak_ptr<member_task_target> lifetime = object;
    {
        auto task = make_task(&member_task_target::read, object);
        object.reset();
        auto result = task();
        if (lifetime.expired() || !result.has_value() || result.value() != 42) {
            return false;
        }
    }
    if (!lifetime.expired()) {
        return false;
    }
    auto task = make_task(&member_task_target::read, std::make_shared<member_task_target>());
    auto result = task();
    return result.has_value() && result.value() == 42;
}

} // namespace

int main() {
    const bool spsc_ok = test_spsc_variadic_emplace();
    const bool result_ok = test_result_error_copy_fallback();
    const bool callable_ok = test_large_byte_aligned_callable();
    const bool member_task_ok = test_shared_member_task();

    std::printf("[%s] spsc variadic emplace\n", spsc_ok ? "OK" : "FAIL");
    std::printf("[%s] result_t error copy fallback\n", result_ok ? "OK" : "FAIL");
    std::printf("[%s] large byte-aligned callable\n", callable_ok ? "OK" : "FAIL");
    std::printf("[%s] shared member task ownership\n", member_task_ok ? "OK" : "FAIL");
    return spsc_ok && result_ok && callable_ok && member_task_ok ? 0 : 1;
}
