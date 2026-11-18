#include "lifecycle_test_support.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <type_traits>

using namespace flux_foundry;
namespace {
using error = lifecycle_test::error;
using output = result_t<int, error>;
struct state {
    task_wrapper_sbo terminal[2];
    int created = 0, destroyed = 0, delivered = 0, value = 0, errors = 0;
    int marker_destroyed = 0;
    void complete(int i) noexcept {
        if (!terminal[i]) std::abort();
        auto callback = std::move(terminal[i]);
        callback();
    }
};
struct marker {
    state* owner;
    explicit marker(state* s) noexcept : owner(s) {}
    ~marker() noexcept { ++owner->marker_destroyed; }
};
struct request { state* owner; int index; int value; };
using input = result_t<request, error>;
struct operation final : awaitable_base<operation, int, error> {
    using async_result_type = output;
    request in;
    explicit operation(input&& r) noexcept : in(r.value()) { ++in.owner->created; }
    ~operation() noexcept { ++in.owner->destroyed; }
    bool available() const noexcept { return true; }
    void cancel() noexcept {}
    int submit() noexcept {
        retain();
        in.owner->terminal[in.index] = task_wrapper_sbo([this]() noexcept {
            resume(output(value_tag, in.value));
            release();
        });
        return 0;
    }
};
struct leaf_transform {
    std::shared_ptr<marker> keep_alive;
    int operator()(int v) const noexcept { return v + 1; }
};
struct receiver {
    using value_type = output;
    state* owner;
    void emplace(output&& out) noexcept {
        ++owner->delivered;
        if (out.has_value()) owner->value = out.value();
        else ++owner->errors;
    }
};
struct sum {
    output operator()(int a, int b) const noexcept { return output(value_tag, a + b); }
};
struct winner {
    output operator()(std::size_t, int v) const noexcept { return output(value_tag, v); }
};
struct failure {
    output operator()(error e) const noexcept { return output(error_tag, std::move(e)); }
};
template <typename... P>
auto aggregate(std::false_type, std::false_type, P... p) { return await_when_all(sum{}, failure{}, std::move(p)...); }
template <typename... P>
auto aggregate(std::true_type, std::false_type, P... p) { return await_when_all_fast(sum{}, failure{}, std::move(p)...); }
template <typename... P>
auto aggregate(std::false_type, std::true_type, P... p) { return await_when_any(winner{}, failure{}, std::move(p)...); }
template <typename... P>
auto aggregate(std::true_type, std::true_type, P... p) { return await_when_any_fast(winner{}, failure{}, std::move(p)...); }

auto leaf(state& s) {
    return make_blueprint<request, error>() | await<operation>()
        | transform(leaf_transform{std::make_shared<marker>(&s)}) | end();
}

template <bool Fast, bool Any>
bool ownership_case() {
    state s;
    std::weak_ptr<marker> weak[2];
    bool ok = true;
    {
        auto m0 = std::make_shared<marker>(&s), m1 = std::make_shared<marker>(&s);
        weak[0] = m0; weak[1] = m1;
        auto a = make_blueprint<request, error>() | await<operation>() | transform(leaf_transform{std::move(m0)}) | end();
        auto b = make_blueprint<request, error>() | await<operation>() | transform(leaf_transform{std::move(m1)}) | end();
        auto p0 = make_lite_ptr<decltype(a)>(std::move(a));
        auto p1 = make_lite_ptr<decltype(b)>(std::move(b));
        auto bp = aggregate(std::integral_constant<bool, Fast>{}, std::integral_constant<bool, Any>{}, p0, p1) | end();
        {
            auto runner = make_fast_runner(std::move(bp), receiver{&s});
            runner(request{&s, 0, 20}, request{&s, 1, 30});
        }
        // One external owner, one reusable factory owner, one child continuation.
        // A consumed per-invocation pack must not retain a duplicate child owner.
        ok = p0.use_count() == 3 && p1.use_count() == 3;
    }
    ok = ok && !weak[0].expired() && !weak[1].expired() && s.delivered == 0;
    s.complete(1);
    if (Any) {
        ok = ok && s.delivered == 1 && s.value == 31;
        // A fast winner leaves the losing child running after its parent's
        // blueprint disappears. That child still owns its callable captures.
        if (Fast) ok = ok && !weak[0].expired();
    } else {
        ok = ok && s.delivered == 0 && !weak[0].expired() && !weak[1].expired();
    }
    s.complete(0);
    ok = ok && s.delivered == 1 && s.errors == 0 && s.value == (Any ? 31 : 52)
        && s.created == 2 && s.destroyed == 2 && s.marker_destroyed == 2
        && weak[0].expired() && weak[1].expired();
    std::printf("[%s] aggregate ownership fast=%d any=%d created=%d destroyed=%d captures=%d\n",
        ok ? "OK" : "FAIL", Fast, Any, s.created, s.destroyed, s.marker_destroyed);
    return ok;
}

template <bool Fast, bool Any>
bool reusable_factory_case() {
    state s;
    bool ok = true;
    {
        auto a = leaf(s);
        auto p = make_lite_ptr<decltype(a)>(std::move(a));
        auto bp = aggregate(std::integral_constant<bool, Fast>{}, std::integral_constant<bool, Any>{}, p, p) | end();
        auto parent = make_lite_ptr<decltype(bp)>(std::move(bp));
        for (int round = 0; round < 2; ++round) {
            {
                auto runner = make_fast_runner(parent, receiver{&s});
                runner(request{&s, 0, 20}, request{&s, 1, 30});
            }
            s.complete(round);
            s.complete(1 - round);
            ok = ok && s.delivered == round + 1 && s.errors == 0
                && s.value == (Any ? (round == 0 ? 21 : 31) : 52)
                && p.use_count() == 3 && s.marker_destroyed == 0;
        }
    }
    ok = ok && s.created == 4 && s.destroyed == 4 && s.marker_destroyed == 1;
    std::printf("[%s] aggregate reusable factory fast=%d any=%d\n", ok ? "OK" : "FAIL", Fast, Any);
    return ok;
}
} // namespace

int main() {
    bool ok = ownership_case<false, false>();
    ok = ownership_case<false, true>() && ok;
    ok = ownership_case<true, false>() && ok;
    ok = ownership_case<true, true>() && ok;
    ok = reusable_factory_case<false, false>() && ok;
    ok = reusable_factory_case<false, true>() && ok;
    ok = reusable_factory_case<true, false>() && ok;
    ok = reusable_factory_case<true, true>() && ok;
    return ok ? 0 : 1;
}
