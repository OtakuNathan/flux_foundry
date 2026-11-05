// FF owns the business pipeline. Dynabridge supplies schema-generated calls.
#include <dynabridge/extensions/flux_foundry/async.h>
#include <dynabridge/backend_base.h>
#include <deque>
#include <cstdio>
#include <limits>
#include <tuple>
#define DYNABRIDGE_IMPORT_DEF "typed_async.def"
#include <dynabridge/import.h>
#undef DYNABRIDGE_IMPORT_DEF

namespace ff = flux_foundry;
namespace db = dynabridge;
namespace {
void check(bool ok) { if (!ok) std::abort(); }

// This demo's application loop is deliberately small. A real backend can
// submit to an existing I/O service; Dynabridge does not choose its scheduler.
struct host_backend : db::backend_base<host_backend> {
    struct context : std::enable_shared_from_this<context> {
        using backend_t = host_backend;
        std::deque<std::function<void()>> ready;
        bool closed = false;
        void poll() {
            auto keep_alive = shared_from_this();
            auto task = std::move(ready.front());
            ready.pop_front();
            task();
        }
        void close() noexcept { closed = true; }
    };
    template<class Receiver, class Direction> struct object_t {};
    template<class Symbol, class Contract, class Receiver, class Tuple>
    static auto prepare_async(std::shared_ptr<Receiver> owner, Tuple arguments);
};
template<class Symbol> struct operation;
template<> struct operation<db::import_symbols::calculator::add> {
    static long long evaluate(int a, int b) noexcept { return static_cast<long long>(a) + b; }
};
template<> struct operation<db::import_symbols::calculator::multiply> {
    static long long evaluate(int a, int b) noexcept { return static_cast<long long>(a) * b; }
};
template<class Symbol, class Contract, class Receiver, class Tuple>
auto host_backend::prepare_async(std::shared_ptr<Receiver> owner, Tuple arguments) {
    return db::make_invocation<int>(std::move(owner),
        [arguments = std::move(arguments)](Receiver& receiver, auto done) DYNABRIDGE_NOEXCEPT_IF_DISABLED {
            auto& host = receiver.context();
            if (host.closed)
                return db::async_submission::reject(std::make_error_code(std::errc::operation_not_permitted));
            // Allocation and completion retention happen before accepting work.
            host.ready.emplace_back([arguments, done]() noexcept {
                const auto result = operation<Symbol>::evaluate(std::get<0>(arguments), std::get<1>(arguments));
                if (result < std::numeric_limits<int>::min() || result > std::numeric_limits<int>::max())
                    done.finish(db::async_result<int>(std::make_error_code(std::errc::result_out_of_range)));
                else
                    done.finish(db::async_result<int>(static_cast<int>(result)));
            });
            return db::async_submission::accept();
        });
}
using output = ff::result_t<int, db::async_error>;
struct observer {
    int calls = 0, value = 0;
    std::error_code error;
};
struct receiver {
    using value_type = output;
    observer* observed;
    void emplace(output&& result) noexcept {
        ++observed->calls;
        if (result.has_value()) observed->value = result.value();
        else observed->error = result.error().code;
    }
};
template<class View>
void run_pipeline(View host_a, View host_b, const std::shared_ptr<host_backend::context>& a,
                  const std::shared_ptr<host_backend::context>& b, const char* mode) {
    using add_call = decltype(host_a.add(20, 1));
    using multiply_call = decltype(host_b.multiply(21, 2));
    auto blueprint = ff::make_blueprint<add_call, db::async_error>()
        | db::ff::await_static_invocation<add_call>()
        | ff::then([host_b](output&& input) DYNABRIDGE_NOEXCEPT_IF_DISABLED {
            return ff::result_t<multiply_call, db::async_error>(ff::value_tag, host_b.multiply(input.value(), 2));
        })
        | db::ff::await_static_invocation<multiply_call>()
        | ff::transform([](int n) noexcept { return n / 2; }) // Business calculation after both awaits.
        | ff::end([](output&& result) noexcept -> output { return std::move(result); });
    auto shared_blueprint = ff::make_lite_ptr<decltype(blueprint)>(std::move(blueprint));
    check(bool(shared_blueprint));
    for (int run = 1; run <= 2; ++run) {
        observer observed;
        auto runner = ff::make_fast_runner(shared_blueprint, receiver{&observed});
        runner(host_a.add(20, run)); // Fresh invocation; the Blueprint is reusable.
        check(observed.calls == 0 && a->ready.size() == 1);
        a->poll();
        check(observed.calls == 0 && b->ready.size() == 1);
        b->poll();
        check(observed.calls == 1 && !observed.error && observed.value == 20 + run);
        std::printf("%s: await Host A -> await Host B -> calc = %d\n", mode, observed.value);
    }
    b->close();
    observer rejected;
    auto runner = ff::make_fast_runner(shared_blueprint, receiver{&rejected});
    runner(host_a.add(20, 1));
    a->poll();
    check(rejected.calls == 1 && rejected.error == std::errc::operation_not_permitted && b->ready.empty());
}
}
int main() {
    using delegate = db::calculator<host_backend::context>;
    auto a = std::make_shared<host_backend::context>(), b = std::make_shared<host_backend::context>();
    run_pipeline(db::bind_static_async<db::calculator>(a, delegate::object_t{}),
                 db::bind_static_async<db::calculator>(b, delegate::object_t{}), a, b, "concrete");
    auto c = std::make_shared<host_backend::context>(), d = std::make_shared<host_backend::context>();
    run_pipeline(db::bind_async<db::calculator>(c, delegate::object_t{}),
                 db::bind_async<db::calculator>(d, delegate::object_t{}), c, d, "erased");
}
