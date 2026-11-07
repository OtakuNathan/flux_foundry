#include "flow/flow.h"

#include <cstdio>
#include <memory>
#include <type_traits>
#include <utility>

namespace end_move_test {
struct counts {
    int constructed = 0;
    int destroyed = 0;
    int copies = 0;
    int moves = 0;
    int live = 0;
};

// Both operations are noexcept: a copy is legal, but default end must move.
struct payload {
    counts* state;
    const int* identity;
    int value;

    explicit payload(counts* s = nullptr, const int* id = nullptr, int v = 42) noexcept
        : state(s), identity(id), value(v) {
        if (state) { ++state->constructed; ++state->live; }
    }
    payload(const payload& other) noexcept
        : payload(other.state, other.identity, other.value) {
        if (state) ++state->copies;
    }
    payload(payload&& other) noexcept
        : payload(other.state, other.identity, other.value) {
        if (state) ++state->moves;
        other.identity = nullptr;
    }
    payload& operator=(const payload&) noexcept = default;
    payload& operator=(payload&&) noexcept = default;
    ~payload() noexcept {
        if (state) { ++state->destroyed; --state->live; }
    }
};
struct copy_only : payload {
    using payload::payload;
    copy_only(const copy_only&) noexcept = default;
    copy_only(copy_only&&) = delete;
    friend void swap(copy_only& lhs, copy_only& rhs) noexcept {
        using std::swap;
        swap(static_cast<payload&>(lhs), static_cast<payload&>(rhs));
    }
};
using owned_payload = std::unique_ptr<payload>;
} // namespace end_move_test

// Normal runners instantiate cancellation paths even when never canceled.
namespace flux_foundry {
template <>
struct cancel_error<end_move_test::payload> {
    static end_move_test::payload make(cancel_kind) noexcept { return end_move_test::payload{}; }
};
template <>
struct cancel_error<end_move_test::owned_payload> {
    static end_move_test::owned_payload make(cancel_kind) noexcept { return {}; }
};
} // namespace flux_foundry

namespace {
namespace ff = flux_foundry;
using end_move_test::counts;
using end_move_test::payload;
using end_move_test::owned_payload;
using end_move_test::copy_only;

static_assert(std::is_nothrow_copy_constructible<payload>::value, "copy must be a viable alternative");
static_assert(std::is_nothrow_move_constructible<payload>::value, "payload moves must not throw");
static_assert(!std::is_copy_constructible<owned_payload>::value, "ownership must be move-only");
static_assert(std::is_nothrow_copy_constructible<copy_only>::value, "copy fallback must not throw");
static_assert(!std::is_move_constructible<copy_only>::value, "copy-only control has a deleted direct move");

int failures = 0;
const char* current_case = nullptr;
int current_runner = 0;
bool current_queued = false;

void check(bool condition, const char* label) {
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s (runner=%d, queued=%d): %s\n",
                    current_case, current_runner, current_queued, label);
    }
}

const payload* address(const payload& in) noexcept { return &in; }
const payload* address(const owned_payload& in) noexcept { return in.get(); }
const payload* address(int) noexcept { return nullptr; }
bool moved_from(const payload& in) noexcept { return in.identity == nullptr; }
bool moved_from(const owned_payload& in) noexcept { return !in; }
bool moved_from(int) noexcept { return true; }

struct observation {
    counts* state;
    const int* identity;
    const payload* original;
    const void* input_address;
    int calls = 0;
    bool success = false;
    bool valid = false;
    bool distinct_result = false;
};

bool matches(const payload& in, const observation& out) noexcept {
    return in.state == out.state && in.identity == out.identity && in.value == 42;
}
bool matches(const owned_payload& in, const observation& out) noexcept {
    return in && in.get() == out.original && matches(*in, out);
}

template <typename T, typename E>
bool matches_value(const ff::result_t<T, E>& in, const observation& out) noexcept {
    return matches(in.value(), out);
}
template <typename E>
bool matches_value(const ff::result_t<void, E>&, const observation&) noexcept { return true; }

template <typename Result>
struct receiver {
    using value_type = Result;
    observation* out;
    void emplace(Result&& result) noexcept {
        ++out->calls;
        out->success = result.has_value();
        out->valid = result.has_value() ? matches_value(result, *out) : matches(result.error(), *out);
        out->distinct_result = static_cast<const void*>(&result) != out->input_address;
    }
};

// A single queued handoff, drained explicitly before a borrowed blueprint dies.
struct manual_executor {
    ff::task_wrapper_sbo pending;
    int dispatches = 0;
    void dispatch(ff::task_wrapper_sbo next) noexcept {
        ++dispatches;
        pending = std::move(next);
    }
    void drain() noexcept {
        ff::task_wrapper_sbo next = std::move(pending);
        next();
    }
};

template <typename Runner, typename Input>
void invoke(Runner& runner, Input& input, observation& out, manual_executor* executor) {
    runner(std::move(input));
    if (executor) {
        check(executor->dispatches == 1 && out.calls == 0, "via defers exactly one delivery");
        executor->drain();
    }
    check(out.calls == 1, "exactly one receiver delivery");
}

template <typename BP, typename Input>
void run(BP&& bp, Input& input, observation& out, manual_executor* executor,
         std::integral_constant<int, 0>) {
    using blueprint = typename std::decay<BP>::type;
    auto owned = ff::make_lite_ptr<blueprint>(std::move(bp));
    auto runner = ff::make_runner(std::move(owned), receiver<typename blueprint::O_t>{&out});
    invoke(runner, input, out, executor);
}

template <typename BP, typename Input>
void run(BP&& bp, Input& input, observation& out, manual_executor* executor,
         std::integral_constant<int, 1>) {
    using blueprint = typename std::decay<BP>::type;
    auto runner = ff::make_fast_runner(std::move(bp), receiver<typename blueprint::O_t>{&out});
    invoke(runner, input, out, executor);
}

template <typename BP, typename Input>
void run(BP&& bp, Input& input, observation& out, manual_executor* executor,
         std::integral_constant<int, 2>) {
    using blueprint = typename std::decay<BP>::type;
    auto runner = ff::make_fast_runner_view(bp, receiver<typename blueprint::O_t>{&out});
    invoke(runner, input, out, executor);
}

template <typename BP, typename Input, typename Runner>
void finish(BP&& bp, Input& input, observation& out, bool queued, Runner runner) {
    if (queued) {
        manual_executor executor;
        auto complete = std::move(bp) | ff::via(&executor) | ff::end();
        run(std::move(complete), input, out, &executor, runner);
    } else {
        auto complete = std::move(bp) | ff::end();
        using result = typename decltype(complete)::O_t;
        using returned = decltype(ff::get<0>(complete.nodes_).f(std::declval<result&&>()));
        static_assert(std::is_same<result, returned>::value, "default end returns the same result by value");
        static_assert(noexcept(ff::get<0>(complete.nodes_).f(std::declval<result&&>())),
                      "default end remains noexcept");
        run(std::move(complete), input, out, nullptr, runner);
    }
}

template <typename Input, typename Runner>
void execute(Input& input, observation& out, bool queued, Runner runner, std::false_type) {
    auto bp = ff::make_blueprint<typename Input::value_type, typename Input::error_type>();
    finish(std::move(bp), input, out, queued, runner);
}

template <typename Input, typename Runner>
void execute(Input& input, observation& out, bool queued, Runner runner, std::true_type) {
    auto bp = ff::make_blueprint<int, typename Input::error_type>()
        | ff::transform([](int) noexcept {});
    finish(std::move(bp), input, out, queued, runner);
}

template <typename Factory, typename Runner, typename Void>
void test_case(const char* name, Factory factory, Runner runner, bool queued, Void is_void, bool copy_fallback = false) {
    current_case = name;
    current_runner = Runner::value;
    current_queued = queued;
    counts state;
    int identity = 42;
    {
        auto input = factory(state, &identity);
        const bool success = input.has_value();
        const payload* original = success ? address(input.value()) : address(input.error());
        observation out{&state, &identity, original, &input};
        const int copies_before = state.copies;
        execute(input, out, queued, runner, is_void);
        check(out.success == success && out.valid, "result category, value and identity survive end");
        check(out.distinct_result, "receiver gets an independent result object");
        if (copy_fallback) {
            check(state.copies > copies_before, "copy-only payload retains its copy fallback");
        } else {
            check(state.copies == 0, "default end and handoff never copy a movable payload");
            check(success ? moved_from(input.value()) : moved_from(input.error()), "input payload is moved from");
        }
    }
    check(state.live == 0 && state.constructed == state.destroyed, "all constructed payloads are destroyed");
}

template <typename Runner>
void test_all(Runner runner) {
    using tracked_result = ff::result_t<payload, payload>;
    using copy_result = ff::result_t<copy_only, payload>;
    using owned_result = ff::result_t<owned_payload, owned_payload>;
    using void_input = ff::result_t<int, payload>;
    using owned_void_input = ff::result_t<int, owned_payload>;
    for (bool queued : {false, true}) {
        test_case("copy-only success", [](counts& s, const int* id) noexcept {
            return copy_result(ff::value_tag, &s, id);
        }, runner, queued, std::false_type{}, true);
        test_case("tracked success", [](counts& s, const int* id) noexcept {
            return tracked_result(ff::value_tag, &s, id);
        }, runner, queued, std::false_type{});
        test_case("tracked error", [](counts& s, const int* id) noexcept {
            return tracked_result(ff::error_tag, &s, id);
        }, runner, queued, std::false_type{});
        test_case("move-only success", [](counts& s, const int* id) noexcept {
            return owned_result(ff::value_tag, owned_payload(new payload(&s, id)));
        }, runner, queued, std::false_type{});
        test_case("move-only error", [](counts& s, const int* id) noexcept {
            return owned_result(ff::error_tag, owned_payload(new payload(&s, id)));
        }, runner, queued, std::false_type{});
        test_case("void success", [](counts&, const int*) noexcept {
            return void_input(ff::value_tag, 42);
        }, runner, queued, std::true_type{});
        test_case("void error", [](counts& s, const int* id) noexcept {
            return void_input(ff::error_tag, &s, id);
        }, runner, queued, std::true_type{});
        test_case("void move-only error", [](counts& s, const int* id) noexcept {
            return owned_void_input(ff::error_tag, owned_payload(new payload(&s, id)));
        }, runner, queued, std::true_type{});
    }
}
} // namespace

int main() {
    test_all(std::integral_constant<int, 0>{});
    test_all(std::integral_constant<int, 1>{});
    test_all(std::integral_constant<int, 2>{});
    std::printf("%s: default end move contracts (compiler exceptions=%d, strict=%d)\n",
                failures == 0 ? "PASS" : "FAIL", FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS,
                FLUX_FOUNDRY_NO_EXCEPTION_STRICT);
    return failures == 0 ? 0 : 1;
}
