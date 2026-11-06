#include "builder_storage_support.h"
#include <cstdio>
using namespace builder_storage_test;

using throwing_copy = calc<false, true>;
using throwing_move = calc<true, false>;
static_assert(!std::is_nothrow_constructible<throwing_copy, throwing_copy&>::value,
              "lvalue storage would copy and may throw");
static_assert(std::is_nothrow_constructible<throwing_copy, throwing_copy&&>::value,
              "rvalue storage uses a safe move");
static_assert(std::is_nothrow_constructible<throwing_move, throwing_move&>::value,
              "a safe initial copy alone is not sufficient");
static_assert(!std::is_nothrow_move_constructible<throwing_move>::value,
              "return values and composition must also have safe moves");
static_assert(noexcept(ff::transform(std::declval<throwing_copy&&>())), "factory remains noexcept");
static_assert(noexcept(ff::then(std::declval<result_callback<false, true>&&>())), "then remains noexcept");
static_assert(noexcept(ff::on_error(std::declval<result_callback<false, true>&&>())), "on_error remains noexcept");
static_assert(noexcept(ff::end(std::declval<result_callback<false, true>&&>())), "end remains noexcept");
static_assert(noexcept(ff::via(std::declval<handle<false, true>&&>())), "via remains noexcept");
static_assert(noexcept(ff::await<awaitable>(std::declval<handle<false, true>&&>())), "await remains noexcept");
using node = decltype(ff::transform(std::declval<throwing_copy&&>()));
static_assert(std::is_nothrow_move_constructible<node>::value, "returned node moves safely");

// Empty and non-copyable callables remain valid, including through EBO storage.
struct move_only {
    move_only() noexcept = default;
    move_only(const move_only&) = delete;
    move_only(move_only&&) noexcept = default;
    int operator()(int n) noexcept { return n; }
};

struct copy_only {
    copy_only() noexcept = default;
    copy_only(const copy_only&) noexcept {}
    int operator()(int n) noexcept { return n; }
};
struct category_sensitive {
    category_sensitive() noexcept = default;
    category_sensitive(category_sensitive&) noexcept {}
    category_sensitive(const category_sensitive&) noexcept(false) { std::abort(); }
    category_sensitive(category_sensitive&&) noexcept = default;
    int operator()(int n) noexcept { return n; }
};
struct final_callable final {
    final_callable() noexcept = default;
    final_callable(final_callable&&) noexcept = default;
    int operator()(int n) noexcept { return n; }
};
int free_function(int n) noexcept { return n; }

// Constructing the argument expression belongs to the caller, not the noexcept
// builder. Its exception must propagate rather than being caught by a builder.
struct caller_constructed {
    explicit caller_constructed(bool fail) noexcept(false) {
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        if (fail) throw 123;
#else
        (void)fail;
#endif
    }
    caller_constructed(caller_constructed&&) noexcept = default;
    int operator()(int n) noexcept { return n; }
};
static_assert(noexcept(ff::transform(std::declval<caller_constructed&&>())),
              "storing an existing rvalue is noexcept");
static_assert(!noexcept(ff::transform(caller_constructed(false))),
              "the full call expression includes argument construction");

#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
struct caller_copied {
    caller_copied() noexcept = default;
    caller_copied(const caller_copied&) { throw 456; }
    caller_copied(caller_copied&&) noexcept = default;
    int operator()(int n) noexcept { return n; }
};
static_assert(!noexcept(ff::transform(caller_copied(std::declval<caller_copied&>()))),
              "an explicit argument copy happens before entering the factory");
#endif

int main() {
    counts c;
    int value = 0;
    // All potentially throwing copy constructors abort if accidentally invoked.
    // More than MAX_ZIP_N calculations exercise both zip and prepend paths.
    auto bp = ff::make_blueprint<int, ff::flow_async_agg_err_t>()
        | ff::transform(calc<false, true>(&c))
        | ff::then(result_callback<false, true>(&c))
        | ff::on_error(result_callback<false, true>(&c))
        | ff::transform(calc<false, true>(&c))
        | ff::via(handle<false, true>(&c))
        | ff::via(handle<false, true>(&c))
        | ff::transform(move_only{})
        | ff::end(result_callback<false, true>(&c));
    static_assert(std::is_nothrow_move_constructible<decltype(bp)>::value, "blueprint moves safely");
    auto owned = ff::make_lite_ptr<decltype(bp)>(std::move(bp));
    auto runner = ff::make_fast_runner(owned, receiver{&value});
    runner(40);
    if (value != 42 || c.copies != 0 || c.moves == 0) return 1;

    // Safe lvalue copy is accepted, while later moves remain nonthrowing.
    counts lvalue_counts;
    calc<true, true> lvalue(&lvalue_counts);
    auto copy_node = ff::transform(lvalue);
    if (copy_node.f(0) != 1 || lvalue_counts.copies != 1) return 2;
    const calc<true, true> const_lvalue;
    (void)ff::transform(const_lvalue);
    auto function = +[](int n) noexcept { return n; };
    (void)ff::transform(function);
    (void)ff::transform(free_function);
    copy_only copyable;
    auto copy_only_bp = ff::make_blueprint<int, ff::flow_async_agg_err_t>()
        | ff::transform(copyable) | ff::end();
    category_sensitive sensitive;
    auto category_bp = ff::make_blueprint<int, ff::flow_async_agg_err_t>()
        | ff::transform(sensitive) | ff::transform(final_callable{}) | ff::end();
    (void)copy_only_bp; (void)category_bp;

    // The executor handle must also survive every wrapper/storage move.
    auto awaiting = ff::make_blueprint<int, ff::flow_async_agg_err_t>()
        | ff::await<awaitable>(handle<false, true>(&c))
        | ff::await<awaitable>(handle<false, true>(&c))
        | ff::transform(calc<false, true>(&c))
        | ff::via(handle<false, true>(&c))
        | ff::await<awaitable>(handle<false, true>(&c)) | ff::end();
    (void)awaiting;
    (void)ff::await<awaitable>();
    ff::lite_ptr<branch> child;
#define FF_STORAGE_EXPLICIT handle<false, true>(&c), success<false, true>(&c), failure<false, true>(&c), child
#define FF_STORAGE_IMPLICIT success<false, true>(&c), failure<false, true>(&c), child
    auto all = ff::await_when_all(FF_STORAGE_EXPLICIT) | ff::end();
    auto all_fast = ff::await_when_all_fast(FF_STORAGE_EXPLICIT) | ff::end();
    auto any = ff::await_when_any(FF_STORAGE_EXPLICIT) | ff::end();
    auto any_fast = ff::await_when_any_fast(FF_STORAGE_EXPLICIT) | ff::end();
    auto all_inline = ff::await_when_all(FF_STORAGE_IMPLICIT) | ff::end();
    auto all_fast_inline = ff::await_when_all_fast(FF_STORAGE_IMPLICIT) | ff::end();
    auto any_inline = ff::await_when_any(FF_STORAGE_IMPLICIT) | ff::end();
    auto any_fast_inline = ff::await_when_any_fast(FF_STORAGE_IMPLICIT) | ff::end();
    (void)all; (void)all_fast; (void)any; (void)any_fast;
    (void)all_inline; (void)all_fast_inline; (void)any_inline; (void)any_fast_inline;
    if (c.copies != 0) return 3;

#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    static_assert(noexcept(ff::catch_exception<std::exception>(
        std::declval<catch_callback<false, true>&&>())), "catch_exception remains noexcept");
    auto catching = ff::make_blueprint<int>()
        | ff::catch_exception<std::exception>(catch_callback<false, true>(&c)) | ff::end();
    (void)catching;
    bool caught = false;
    try {
        (void)ff::transform(caller_constructed(true));
    } catch (int n) {
        caught = n == 123;
    }
    if (!caught) return 4;
    caller_copied original;
    caught = false;
    try {
        (void)ff::transform(caller_copied(original));
    } catch (int n) {
        caught = n == 456;
    }
    if (!caught) return 5;
#endif
    std::printf("[OK] builder storage, composition, C++14 return moves and caller boundary\n");
}
