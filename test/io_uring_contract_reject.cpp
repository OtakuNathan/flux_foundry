#include "extension/io_uring_awaitable.h"

struct good_operation {
    void prepare(io_uring_sqe*) & noexcept {}
    void prepare(int) noexcept {} // Overloads remain valid with expression detection.
};

#if FF_IO_CASE == 1
struct operation {};
#elif FF_IO_CASE == 2 || FF_IO_CASE == 13
struct operation { int prepare(io_uring_sqe*) noexcept { return 0; } };
#elif FF_IO_CASE == 3
struct operation { void prepare(io_uring_sqe*) noexcept(false) {} };
#elif FF_IO_CASE == 4
struct operation { void prepare(int) noexcept {} };
#elif FF_IO_CASE == 5
struct operation : good_operation { operation(operation&&) noexcept(false) {} };
#elif FF_IO_CASE == 6
struct operation : good_operation { ~operation() noexcept(false) {} };
#elif FF_IO_CASE == 7
using operation = good_operation&;
#elif FF_IO_CASE == 11
struct operation { void prepare(io_uring_sqe*) && noexcept {} };
#elif FF_IO_CASE == 12
using operation = const good_operation;
#else
using operation = good_operation;
#endif

#if FF_IO_CASE >= 8 && FF_IO_CASE <= 10
struct error {};
namespace flux_foundry { namespace extension { namespace detail {
#if FF_IO_CASE == 9
template <> struct io_error<error> { static int make(int) noexcept { return 0; } };
#elif FF_IO_CASE == 10
template <> struct io_error<error> { static error make(int) noexcept(false) { return {}; } };
#endif
}}}
#else
using error = flux_foundry::flow_async_agg_err_t;
#endif

#if FF_IO_CASE == 13
// The raw context entry point must enforce the prepare contract too.
void check_context(flux_foundry::extension::io_uring_context& context) {
    operation op;
    context.enqueue(op, [](void*, int, unsigned) noexcept {}, nullptr);
}
#else
static_assert(sizeof(flux_foundry::extension::io_uring_awaitable<operation, error>) > 0,
              "instantiate the awaitable contract at the public boundary");
#endif
