#include <system_error>
#include <utility>
#include <cstdio>

#include "flow/flow.h"

namespace flux_foundry {
template <>
struct cancel_error<std::error_code> {
    static std::error_code make(cancel_kind kind) noexcept {
        return std::error_code(kind == cancel_kind::hard ? 1001 : 1002, std::generic_category());
    }
};

template <>
struct awaitable_creating_error<std::error_code> {
    static std::error_code make() noexcept {
        return std::error_code(1003, std::generic_category());
    }
};

template <>
struct async_submission_failed_error<std::error_code> {
    static std::error_code make() noexcept {
        return std::error_code(1004, std::generic_category());
    }
};

template <>
struct async_all_failed_error<std::error_code> {
    static std::error_code make() noexcept {
        return std::error_code(1005, std::generic_category());
    }
};

template <>
struct async_any_failed_error<std::error_code> {
    static std::error_code make(size_t i) noexcept {
        return std::error_code(static_cast<int>(1100 + i), std::generic_category());
    }
};
}

using namespace flux_foundry;

struct ex {
    void dispatch(task_wrapper_sbo t) noexcept { t(); }
};

struct aw_noexcept : awaitable_base<aw_noexcept, int, std::error_code> {
    using async_result_type = result_t<int, std::error_code>;
    int v;

    explicit aw_noexcept(async_result_type&& in) noexcept
        : v(in.has_value() ? in.value() : 0) {}

    int submit() noexcept {
        this->resume(async_result_type(value_tag, v + 1));
        return 0;
    }

    void cancel() noexcept {}

    bool available() const noexcept { return true; }
};

struct recv {
    using value_type = result_t<int, std::error_code>;
    int* out;
    int* ec;

    void emplace(value_type&& r) noexcept {
        if (r.has_value()) {
            *out = r.value();
        } else {
            *ec = r.error().value();
        }
    }
};

int main() {
    ex e;
    int out = 0;
    int ec = 0;

    auto bp = make_blueprint<int, std::error_code>()
        | await<aw_noexcept>(&e)
        | await<aw_noexcept>(&e)
        | end();

    auto p = make_lite_ptr<decltype(bp)>(std::move(bp));
    auto r = make_runner(p, recv{&out, &ec});
    r(10);

    std::printf("out=%d ec=%d\n", out, ec);
    return (out == 12 && ec == 0) ? 0 : 1;
}
