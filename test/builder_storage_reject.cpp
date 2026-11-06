#include "builder_storage_support.h"
using namespace builder_storage_test;

// Kind 0: throwing lvalue copy, safe later moves.
// Kind 1: safe initial lvalue copy, throwing later moves.
// Kind 2: throwing initial rvalue move.
#if FF_STORAGE_KIND == 0
constexpr bool copy_safe = false;
constexpr bool move_safe = true;
#define FF_STORAGE_ARG(x) (x)
#elif FF_STORAGE_KIND == 1
constexpr bool copy_safe = true;
constexpr bool move_safe = false;
#define FF_STORAGE_ARG(x) (x)
#elif FF_STORAGE_KIND == 2
constexpr bool copy_safe = true;
constexpr bool move_safe = false;
#define FF_STORAGE_ARG(x) std::move(x)
#else
constexpr bool copy_safe = true;
constexpr bool move_safe = true;
#define FF_STORAGE_ARG(x) (x)
#endif

int main() {
#if FF_STORAGE_CASE == 1
    calc<copy_safe, move_safe> f;
    (void)ff::transform(FF_STORAGE_ARG(f));
#elif FF_STORAGE_CASE == 2
    result_callback<copy_safe, move_safe> f;
    (void)ff::then(FF_STORAGE_ARG(f));
#elif FF_STORAGE_CASE == 3
    result_callback<copy_safe, move_safe> f;
    (void)ff::on_error(FF_STORAGE_ARG(f));
#elif FF_STORAGE_CASE == 4
    result_callback<copy_safe, move_safe> f;
    (void)ff::end(FF_STORAGE_ARG(f));
#elif FF_STORAGE_CASE == 5
    catch_callback<copy_safe, move_safe> f;
    (void)ff::catch_exception<std::exception>(FF_STORAGE_ARG(f));
#elif FF_STORAGE_CASE == 6
    handle<copy_safe, move_safe> h;
    (void)ff::via(FF_STORAGE_ARG(h));
#elif FF_STORAGE_CASE == 7
    handle<copy_safe, move_safe> h;
    (void)ff::await<awaitable>(FF_STORAGE_ARG(h));
#else
    // Cases 8..27: four aggregation factories, each with an explicit unsafe
    // executor, success or error callback, and two implicit-executor callbacks.
    ff::lite_ptr<branch> bp;
#if (FF_STORAGE_CASE - 8) % 5 == 0
    handle<copy_safe, move_safe> h;
    success<true, true> f;
    failure<true, true> g;
#define FF_STORAGE_AGG_ARGS FF_STORAGE_ARG(h), f, g, bp
#elif (FF_STORAGE_CASE - 8) % 5 == 1
    handle<true, true> h;
    success<copy_safe, move_safe> f;
    failure<true, true> g;
#define FF_STORAGE_AGG_ARGS h, FF_STORAGE_ARG(f), g, bp
#elif (FF_STORAGE_CASE - 8) % 5 == 2
    handle<true, true> h;
    success<true, true> f;
    failure<copy_safe, move_safe> g;
#define FF_STORAGE_AGG_ARGS h, f, FF_STORAGE_ARG(g), bp
#elif (FF_STORAGE_CASE - 8) % 5 == 3
    success<copy_safe, move_safe> f;
    failure<true, true> g;
#define FF_STORAGE_AGG_ARGS FF_STORAGE_ARG(f), g, bp
#else
    success<true, true> f;
    failure<copy_safe, move_safe> g;
#define FF_STORAGE_AGG_ARGS f, FF_STORAGE_ARG(g), bp
#endif
#if FF_STORAGE_CASE < 13
    (void)ff::await_when_all(FF_STORAGE_AGG_ARGS);
#elif FF_STORAGE_CASE < 18
    (void)ff::await_when_all_fast(FF_STORAGE_AGG_ARGS);
#elif FF_STORAGE_CASE < 23
    (void)ff::await_when_any(FF_STORAGE_AGG_ARGS);
#else
    (void)ff::await_when_any_fast(FF_STORAGE_AGG_ARGS);
#endif
#endif
}
