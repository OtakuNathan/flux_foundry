#include <cstdio>
#include <memory>
#include <stdexcept>
#include <utility>

#include "flow/flow.h"

using namespace flux_foundry;

namespace {
using output = result_t<int, std::exception_ptr>;
using continuation = callable_wrapper<void(output&&)>;

template <template <typename, typename, typename> class Base>
struct awaitable final : Base<awaitable<Base>, int, std::exception_ptr> {
    int submit() noexcept { this->resume(output(value_tag, 42)); return 0; }
    void cancel() noexcept {}
};

struct converted_callback {
    continuation value;
    operator continuation() && noexcept { return std::move(value); }
};

struct observation { int result = 0; int destroyed = 0; };

template <std::size_t Padding>
struct owned_callback {
    char padding[Padding]{};
    std::unique_ptr<int> value;
    observation* state;
    owned_callback** exposed;
    owned_callback(observation* s, owned_callback** p) noexcept
        : value(new int(7)), state(s), exposed(p) { *exposed = this; }
    owned_callback(owned_callback&& rhs) noexcept
        : value(std::move(rhs.value)), state(rhs.state), exposed(rhs.exposed) { *exposed = this; }
    owned_callback(const owned_callback&) = delete;
    ~owned_callback() { if (value) ++state->destroyed; }
    void operator()(output&& out) noexcept { state->result = out.value() + *value; }
};

template <typename Awaitable, std::size_t Padding>
bool owned_alias_replacement() {
    observation state;
    owned_callback<Padding>* stored = nullptr;
    auto delegate = (new Awaitable)->delegate();
    owned_callback<Padding> initial(&state, &stored);
    delegate.emplace_nextstep(std::move(initial));
    // The replacement is the payload currently owned by the destination.
    delegate.emplace_nextstep(std::move(*stored));
    if (state.destroyed || initial.value) { delegate.release(); return false; }
    const int code = delegate.submit_async();
    return code == 0 && state.result == 49 && state.destroyed == 1;
}

template <typename Awaitable>
bool lvalue_and_erased_callbacks() {
    observation state;
    auto delegate = (new Awaitable)->delegate();
    auto callback = [&state](output&& out) noexcept { state.result = out.value(); };
    delegate.emplace_nextstep(callback);
    continuation erased([&state](output&& out) noexcept { state.result = out.value() + 1; });
    delegate.emplace_nextstep(std::move(erased));
    if (erased) { delegate.release(); return false; }
    // Empty wrappers must clear the callback, without wrapping an empty wrapper.
    delegate.emplace_nextstep(std::move(erased));
    delegate.emplace_nextstep(callback);
    // Conversion-only inputs must still use the original erased overload.
    converted_callback converted{continuation(callback)};
    delegate.emplace_nextstep(std::move(converted));
    if (converted.value) { delegate.release(); return false; }
    return delegate.submit_async() == 0 && state.result == 42;
}

template <typename Awaitable, std::size_t Padding>
bool clear_owned_callback() {
    observation state;
    owned_callback<Padding>* stored = nullptr;
    auto delegate = (new Awaitable)->delegate();
    delegate.emplace_nextstep(owned_callback<Padding>(&state, &stored));
    continuation empty;
    delegate.emplace_nextstep(std::move(empty));
    const bool cleared = state.destroyed == 1;
    delegate.release();
    return cleared && state.destroyed == 1;
}

#if FLUX_FOUNDRY_HAS_EXCEPTIONS
// A custom delegate using only the original erased-callback signature.
template <bool Inline>
struct legacy_awaitable final : fast_awaitable_base<legacy_awaitable<Inline>, int, std::exception_ptr> {
    using base = fast_awaitable_base<legacy_awaitable<Inline>, int, std::exception_ptr>;
    using async_result_type = output;
    static constexpr bool completes_inline = Inline;
    explicit legacy_awaitable(output&&) noexcept {}
    int submit() noexcept { this->resume(output(value_tag, 42)); return 0; }
    struct access_delegate : base::access_delegate {
        explicit access_delegate(typename base::access_delegate d) noexcept : base::access_delegate(d) {}
        void emplace_nextstep(continuation&& callback) noexcept {
            base::access_delegate::emplace_nextstep(std::move(callback));
        }
    };
    access_delegate delegate() noexcept { return access_delegate(base::delegate()); }
};
struct receiver {
    using value_type = output;
    int* result;
    void emplace(output&& out) noexcept { *result = out.value(); }
};
template <bool Inline>
bool legacy_delegate() {
    auto bp = make_blueprint<int>() | await<legacy_awaitable<Inline>>() | end();
    auto ptr = make_lite_ptr<decltype(bp)>(std::move(bp));
    int normal_result = 0, fast_result = 0;
    auto normal = make_runner(ptr, receiver{&normal_result});
    auto fast = make_fast_runner(ptr, receiver{&fast_result});
    normal(0);
    fast(0);
    return normal_result == 42 && fast_result == 42;
}

template <std::size_t Padding>
struct throwing_copy {
    char padding[Padding]{};
    int* result;
    bool* fail;
    throwing_copy(int* r, bool* f) noexcept : result(r), fail(f) {}
    throwing_copy(const throwing_copy& rhs) : result(rhs.result), fail(rhs.fail) {
        if (*fail) throw std::runtime_error("copy failed");
    }
    throwing_copy(throwing_copy&&) noexcept = default;
    void operator()(output&& out) noexcept { *result = out.value() + 2; }
};

template <typename Awaitable, std::size_t Padding>
bool construction_failure(bool occupied) {
    int result = 0;
    bool fail = true, caught = false;
    auto delegate = (new Awaitable)->delegate();
    if (occupied) delegate.emplace_nextstep([&result](output&& out) noexcept { result = out.value(); });
    throwing_copy<Padding> callback(&result, &fail);
    static_assert(!noexcept(delegate.emplace_nextstep(callback)), "copy failure must propagate");
    try { delegate.emplace_nextstep(callback); }
    catch (const std::runtime_error&) { caught = true; }
    if (!caught) { delegate.release(); return false; }
    if (!occupied) {
        fail = false;
        delegate.emplace_nextstep(callback);
    }
    return delegate.submit_async() == 0 && result == (occupied ? 42 : 44);
}
#endif

template <typename Awaitable>
bool check_delegate() {
    using delegate = typename Awaitable::access_delegate;
    static_assert(noexcept(std::declval<delegate>().emplace_nextstep(std::declval<continuation&&>())),
                  "erased callback transfer remains noexcept");
    bool ok = owned_alias_replacement<Awaitable, 1>() && owned_alias_replacement<Awaitable, 128>() &&
              lvalue_and_erased_callbacks<Awaitable>() &&
              clear_owned_callback<Awaitable, 1>() && clear_owned_callback<Awaitable, 128>();
#if FLUX_FOUNDRY_HAS_EXCEPTIONS
    ok = construction_failure<Awaitable, 1>(false) && construction_failure<Awaitable, 1>(true) &&
         construction_failure<Awaitable, 128>(false) && construction_failure<Awaitable, 128>(true) && ok;
#endif
    return ok;
}
} // namespace

int main() {
    const bool normal = check_delegate<awaitable<awaitable_base>>();
    const bool fast = check_delegate<awaitable<fast_awaitable_base>>();
    std::printf("[%s] normal continuation ownership and replacement\n", normal ? "OK" : "FAIL");
    std::printf("[%s] fast continuation ownership and replacement\n", fast ? "OK" : "FAIL");
    bool ok = normal && fast;
#if FLUX_FOUNDRY_HAS_EXCEPTIONS
    const bool legacy = legacy_delegate<false>() && legacy_delegate<true>();
    std::printf("[%s] legacy erased-only delegates with both runners\n", legacy ? "OK" : "FAIL");
    ok = legacy && ok;
#endif
    return ok ? 0 : 1;
}
