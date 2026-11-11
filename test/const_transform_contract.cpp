#include "flow/flow.h"

#include <memory>
#include <system_error>
#include <type_traits>

#define CHECK(expr) do { if (!(expr)) return __LINE__; } while (false)

namespace ff = flux_foundry;
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
using error_type = std::exception_ptr;
#else
using error_type = std::error_code;
#endif
using result_type = ff::result_t<int, error_type>;
using void_result = ff::result_t<void, error_type>;

struct Pure { constexpr int operator()(int x) const noexcept { return x + 3; } };
struct Add { int n; int operator()(int x) const noexcept { return x + n; } };
struct Mutable { int n = 0; int operator()(int x) noexcept { return x + (++n); } };
struct Dual {
    int operator()(int x) noexcept { return x + 1; }
    int operator()(int x) const noexcept { return x + 2; }
};
struct Different {
    int operator()(int x) noexcept { return x; }
    long operator()(int x) const noexcept { return x; }
};
struct ThrowsConst {
    int operator()(int x) noexcept { return x; }
    int operator()(int x) const { return x; }
};
struct Move { int operator()(std::unique_ptr<int> x) const noexcept { return *x; } };
struct VoidIn { int operator()() const noexcept { return 17; } };
struct VoidOut { void operator()(int) const noexcept {} };
struct MutableVoidIn { int operator()() noexcept { return 17; } };

template <class F>
using wrapper = ff::flow_impl::transform_wrapper<F, int, error_type>;
using mutable_void_wrapper = ff::flow_impl::transform_wrapper<MutableVoidIn, void, error_type>;
static_assert(ff::flow_impl::is_invocable_with<const wrapper<Pure>, result_type&&>::value, "pure const");
static_assert(!ff::flow_impl::is_invocable_with<const wrapper<Mutable>, result_type&&>::value, "mutable rejected");
static_assert(!ff::flow_impl::is_invocable_with<const wrapper<Different>, result_type&&>::value, "different result rejected");
static_assert(!ff::flow_impl::is_invocable_with<const wrapper<ThrowsConst>, result_type&&>::value, "throwing const rejected");
static_assert(!ff::flow_impl::is_invocable_with<const mutable_void_wrapper, void_result&&>::value, "mutable void rejected");
using mutable_zip = ff::flow_impl::zipped_callable<ff::flow_impl::identity, wrapper<Mutable>>;
static_assert(!ff::flow_impl::is_invocable_with<const mutable_zip, result_type&&>::value, "const zip SFINAE");
static_assert(!std::is_copy_assignable<wrapper<Pure>>::value, "preserve captured-lambda nonassignability");
static_assert(!std::is_move_assignable<wrapper<Pure>>::value, "preserve captured-lambda nonassignability");
static_assert(sizeof(wrapper<Pure>) == 1, "empty storage");
static_assert(sizeof(wrapper<Mutable>) == sizeof(Mutable), "state only");

struct ReturnReference {
    int* p;
    int& operator()(int) const noexcept { return *p; }
};
using reference_zip = ff::flow_impl::zipped_callable<ff::flow_impl::identity, ReturnReference>;
static_assert(std::is_same<decltype(std::declval<reference_zip&>()(1)), int>::value, "zip retains auto value return");
static_assert(std::is_same<decltype(std::declval<const reference_zip&>()(1)), int>::value, "const zip retains auto value return");

struct Receiver {
    using value_type = result_type;
    int* p;
    void emplace(result_type&& r) noexcept { *p = r.has_value() ? r.value() : -1; }
};

#if FLUX_FOUNDRY_CPP_VER >= 201703L
constexpr
#endif
auto build_long_blueprint() {
    // Cross the fusion boundary with stored state; construction never calls Add.
    return ff::make_blueprint<int, error_type>()
        | ff::transform(Add{1}) | ff::transform(Add{2})
        | ff::transform(Add{3}) | ff::transform(Add{4})
        | ff::transform(Add{5}) | ff::transform(Add{6})
        | ff::transform(Add{7}) | ff::transform(Add{8})
        | ff::transform(Add{9}) | ff::transform(Add{10}) | ff::end();
}

int main() {
    int source = 5;
    reference_zip zip(ff::flow_impl::identity{}, ReturnReference{&source});
    auto&& copied = zip(1);
    copied = 9;
    CHECK(source == 5);

    wrapper<Mutable> mutable_fn(Mutable{});
    CHECK(mutable_fn(result_type(ff::value_tag, 2)).value() == 3);
    CHECK(mutable_fn(result_type(ff::value_tag, 2)).value() == 4);
    wrapper<Dual> dual(Dual{});
    CHECK(dual(result_type(ff::value_tag, 2)).value() == 3);
    const auto& const_dual = dual;
    CHECK(const_dual(result_type(ff::value_tag, 2)).value() == 4);
    const wrapper<Pure> pure(Pure{});
    CHECK(pure(result_type(ff::error_tag, error_type{})).has_error());

    const ff::flow_impl::transform_wrapper<Move, std::unique_ptr<int>, error_type> move(Move{});
    CHECK(move(ff::result_t<std::unique_ptr<int>, error_type>(
        ff::value_tag, std::unique_ptr<int>(new int(9)))).value() == 9);
    const ff::flow_impl::transform_wrapper<VoidIn, void, error_type> void_in(VoidIn{});
    CHECK(void_in(void_result(ff::value_tag)).value() == 17);
    const wrapper<VoidOut> void_out(VoidOut{});
    CHECK(void_out(result_type(ff::value_tag, 1)).has_value());

    int result = 0;
#if FLUX_FOUNDRY_CPP_VER >= 201703L
    constexpr
#else
    const
#endif
    auto bp = ff::make_blueprint<int, error_type>() | ff::transform(Pure{}) | ff::end();
    auto runner = ff::make_fast_runner_view(bp, Receiver{&result});
    runner(4); CHECK(result == 7);
    runner(8); CHECK(result == 11);

#if FLUX_FOUNDRY_CPP_VER >= 201703L
    constexpr
#else
    const
#endif
    auto long_bp = build_long_blueprint();
    auto long_runner = ff::make_fast_runner_view(&long_bp, Receiver{&result});
    long_runner(4); CHECK(result == 59);
    long_runner(8); CHECK(result == 63);

#if FLUX_FOUNDRY_CPP_VER >= 201703L
    constexpr
#else
    const
#endif
    auto void_bp = ff::make_blueprint<int, error_type>()
        | ff::transform(VoidOut{}) | ff::transform(VoidIn{}) | ff::end();
    auto void_runner = ff::make_fast_runner_view(void_bp, Receiver{&result});
    void_runner(4); CHECK(result == 17);

    auto mutable_bp = ff::make_blueprint<int, error_type>()
        | ff::transform([n = 0](int x) mutable noexcept { return x + (++n); }) | ff::end();
    auto mutable_runner = ff::make_fast_runner_view(mutable_bp, Receiver{&result});
    mutable_runner(4); CHECK(result == 5);
    mutable_runner(4); CHECK(result == 6);
}
