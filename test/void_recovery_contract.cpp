#include "flow/flow.h"

#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>

// Normal runners require the documented cancellation-error adapter for custom E.
#if !FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
namespace flux_foundry {
template <>
struct cancel_error<std::error_code> {
    static std::error_code make(cancel_kind) noexcept {
        return std::make_error_code(std::errc::operation_canceled);
    }
};
}
#endif

namespace ff = flux_foundry;

namespace {
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
using error_type = std::exception_ptr;
error_type make_error() { return std::make_exception_ptr(std::runtime_error("source")); }
#else
using error_type = std::error_code;
error_type make_error() noexcept { return std::make_error_code(std::errc::invalid_argument); }
#endif
using int_result = ff::result_t<int, error_type>;
using void_result = ff::result_t<void, error_type>;
using ptr_result = ff::result_t<std::unique_ptr<int>, error_type>;

int failures = 0;
void check(bool valid, const char* label) {
    if (!valid) {
        ++failures;
        std::printf("FAIL: %s\n", label);
    }
}

template <typename BP>
void run(BP&& bp, int input, std::true_type) {
    auto runner = ff::make_fast_runner(std::move(bp));
    runner(input);
}

template <typename BP>
void run(BP&& bp, int input, std::false_type) {
    using blueprint_type = typename std::decay<BP>::type;
    auto owned = ff::make_lite_ptr<blueprint_type>(std::move(bp));
    auto runner = ff::make_runner(std::move(owned));
    runner(input);
}

template <typename Runner>
void test_void_on_error(Runner runner) {
    // A valid typed entry followed by a void transform must skip recovery.
    int transforms = 0, handlers = 0, terminals = 0;
    bool success = false;
    auto bypass = ff::make_blueprint<int, error_type>()
        | ff::transform([&](int) noexcept { ++transforms; })
        | ff::on_error([&](void_result&&) noexcept -> void_result {
            ++handlers;
            return void_result(ff::value_tag);
        })
        | ff::end([&](void_result&& in) noexcept -> void_result {
            ++terminals;
            success = in.has_value();
            return std::move(in);
        });
    run(std::move(bypass), 7, runner);
    check(transforms == 1 && handlers == 0 && terminals == 1 && success,
          "void transform success bypasses on_error");

    const error_type source = make_error();
    for (int mode = 0; mode != 3; ++mode) {
        handlers = 0;
        terminals = 0;
        success = false;
        bool received_source = false;
        error_type output_error{};
        auto bp = ff::make_blueprint<int, error_type>()
            | ff::then([&](int_result&&) noexcept -> void_result {
                return mode == 0 ? void_result(ff::value_tag) : void_result(ff::error_tag, source);
            })
            | ff::on_error([state = std::unique_ptr<int>(new int(40)),
                            &handlers, &received_source, &source, mode](void_result&& in) mutable noexcept -> void_result {
                ++handlers;
                received_source = in.has_error() && in.error() == source && ++*state == 41;
                if (mode == 2) return std::move(in);
                return void_result(ff::value_tag);
            })
            | ff::end([&](void_result&& in) noexcept -> void_result {
                ++terminals;
                success = in.has_value();
                if (in.has_error()) output_error = in.error();
                return std::move(in);
            });
        run(std::move(bp), 7, runner);
        check(terminals == 1 && handlers == (mode == 0 ? 0 : 1), "void on_error call counts");
        check(success == (mode != 2), "void on_error success/error result");
        check(mode == 0 || received_source, "void on_error receives original error and mutable move-only handler");
        check(mode != 2 || output_error == source, "void on_error preserves forwarded error");
    }

#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    handlers = 0;
    terminals = 0;
    bool replacement = false;
    auto throws = ff::make_blueprint<int>()
        | ff::then([](int_result&&) -> void_result { throw std::runtime_error("source"); })
        | ff::on_error([&](void_result&&) -> void_result {
            ++handlers;
            throw std::logic_error("replacement");
        })
        | ff::end([&](void_result&& in) noexcept -> void_result {
            ++terminals;
            if (in.has_error()) {
                try { std::rethrow_exception(in.error()); }
                catch (const std::logic_error&) { replacement = true; }
                catch (...) {}
            }
            return std::move(in);
        });
    run(std::move(throws), 7, runner);
    check(handlers == 1 && terminals == 1 && replacement, "void on_error captures handler exception");
#endif
}

template <typename Runner>
void test_typed_on_error(Runner runner) {
    for (bool fail : {false, true}) {
        const error_type source = make_error();
        int handlers = 0, terminals = 0, value = 0;
        auto typed = ff::make_blueprint<int, error_type>()
            | ff::then([&](int_result&& in) noexcept -> int_result {
                return fail ? int_result(ff::error_tag, source) : std::move(in);
            })
            | ff::on_error([&](int_result&&) noexcept -> ff::result_t<long, error_type> {
                ++handlers;
                return ff::result_t<long, error_type>(ff::value_tag, 42L);
            })
            | ff::end([&](ff::result_t<long, error_type>&& in) noexcept -> ff::result_t<long, error_type> {
                ++terminals;
                value = in.has_value() ? static_cast<int>(in.value()) : -1;
                return std::move(in);
            });
        run(std::move(typed), 7, runner);
        check(handlers == (fail ? 1 : 0) && terminals == 1 && value == (fail ? 42 : 7),
              "typed on_error preserves convertible success/recovery");

        handlers = 0;
        terminals = 0;
        value = 0;
        bool same_pointer = false;
        int* original = nullptr;
        auto movable = ff::make_blueprint<int, error_type>()
            | ff::then([&](int_result&& in) noexcept -> ptr_result {
                if (fail) return ptr_result(ff::error_tag, source);
                std::unique_ptr<int> ptr(new int(in.value()));
                original = ptr.get();
                return ptr_result(ff::value_tag, std::move(ptr));
            })
            | ff::on_error([&](ptr_result&&) noexcept -> ptr_result {
                ++handlers;
                return ptr_result(ff::value_tag, std::unique_ptr<int>(new int(42)));
            })
            | ff::end([&](ptr_result&& in) noexcept -> ptr_result {
                ++terminals;
                if (in.has_value() && in.value()) {
                    value = *in.value();
                    same_pointer = in.value().get() == original;
                }
                return std::move(in);
            });
        run(std::move(movable), 7, runner);
        check(handlers == (fail ? 1 : 0) && terminals == 1 && value == (fail ? 42 : 7),
              "move-only on_error success/recovery");
        check(fail || same_pointer, "move-only on_error bypass preserves ownership");
    }
}

#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
template <typename Runner>
void test_void_catch(Runner runner) {
    const auto matched = std::make_exception_ptr(std::runtime_error("source"));
    const auto unmatched = std::make_exception_ptr(std::logic_error("unmatched"));
    // 0: bypass; 1: matching recovery; 2: nonmatching propagation; 3: handler throws.
    for (int mode = 0; mode != 4; ++mode) {
        int handlers = 0, terminals = 0;
        bool success = false, replacement = false, handler_state = false;
        std::exception_ptr output_error;
        auto bp = ff::make_blueprint<int>()
            | ff::transform([](int) noexcept {})
            | ff::then([&](void_result&& in) noexcept -> void_result {
                if (mode == 0) return std::move(in);
                return void_result(ff::error_tag, mode == 2 ? unmatched : matched);
            })
            | ff::catch_exception<std::runtime_error>(
                [state = std::unique_ptr<int>(new int(40)), &handlers, &handler_state, mode]
                (const std::runtime_error&) mutable {
                    ++handlers;
                    handler_state = ++*state == 41;
                    if (mode == 3) throw std::range_error("replacement");
                })
            | ff::end([&](void_result&& in) noexcept -> void_result {
                ++terminals;
                success = in.has_value();
                if (in.has_error()) {
                    output_error = in.error();
                    try { std::rethrow_exception(in.error()); }
                    catch (const std::range_error&) { replacement = true; }
                    catch (...) {}
                }
                return std::move(in);
            });
        run(std::move(bp), 7, runner);
        const bool handled = mode == 1 || mode == 3;
        check(terminals == 1 && handlers == (handled ? 1 : 0), "void catch_exception call counts");
        check(!handled || handler_state, "void catch_exception mutable move-only handler");
        check(success == (mode < 2), "void catch_exception success/error result");
        check(mode != 2 || output_error == unmatched, "void catch_exception preserves unmatched exception");
        check(mode != 3 || replacement, "void catch_exception captures handler exception");
    }
}

template <typename Runner>
void test_typed_catch(Runner runner) {
    const auto unmatched = std::make_exception_ptr(std::logic_error("unmatched"));
    int unmatched_handlers = 0, unmatched_terminals = 0;
    std::exception_ptr output_error;
    auto unmatched_bp = ff::make_blueprint<int>()
        | ff::then([&](int_result&&) noexcept -> int_result {
            return int_result(ff::error_tag, unmatched);
        })
        | ff::catch_exception<std::runtime_error>([&](const std::runtime_error&) noexcept -> long {
            ++unmatched_handlers;
            return 42;
        })
        | ff::end([&](ff::result_t<long, error_type>&& in) noexcept -> ff::result_t<long, error_type> {
            ++unmatched_terminals;
            if (in.has_error()) output_error = in.error();
            return std::move(in);
        });
    run(std::move(unmatched_bp), 7, runner);
    check(unmatched_handlers == 0 && unmatched_terminals == 1,
          "typed catch_exception forwards unmatched exception without calling handler");
    check(output_error == unmatched, "typed catch_exception preserves unmatched exception");

    for (bool fail : {false, true}) {
        int handlers = 0, terminals = 0, value = 0;
        auto typed = ff::make_blueprint<int>()
            | ff::then([&](int_result&& in) -> int_result {
                if (fail) throw std::runtime_error("source");
                return std::move(in);
            })
            | ff::catch_exception<std::runtime_error>([&](const std::runtime_error&) noexcept -> long {
                ++handlers;
                return 42;
            })
            | ff::end([&](ff::result_t<long, error_type>&& in) noexcept -> ff::result_t<long, error_type> {
                ++terminals;
                value = in.has_value() ? static_cast<int>(in.value()) : -1;
                return std::move(in);
            });
        run(std::move(typed), 7, runner);
        check(handlers == (fail ? 1 : 0) && terminals == 1 && value == (fail ? 42 : 7),
              "typed catch_exception preserves convertible success/recovery");

        handlers = 0;
        terminals = 0;
        value = 0;
        bool same_pointer = false;
        int* original = nullptr;
        auto movable = ff::make_blueprint<int>()
            | ff::then([&](int_result&& in) -> ptr_result {
                if (fail) throw std::runtime_error("source");
                std::unique_ptr<int> ptr(new int(in.value()));
                original = ptr.get();
                return ptr_result(ff::value_tag, std::move(ptr));
            })
            | ff::catch_exception<std::runtime_error>([&](const std::runtime_error&) noexcept {
                ++handlers;
                return std::unique_ptr<int>(new int(42));
            })
            | ff::end([&](ptr_result&& in) noexcept -> ptr_result {
                ++terminals;
                if (in.has_value() && in.value()) {
                    value = *in.value();
                    same_pointer = in.value().get() == original;
                }
                return std::move(in);
            });
        run(std::move(movable), 7, runner);
        check(handlers == (fail ? 1 : 0) && terminals == 1 && value == (fail ? 42 : 7),
              "move-only catch_exception success/recovery");
        check(fail || same_pointer, "move-only catch_exception bypass preserves ownership");
    }
}
#endif

template <typename Runner>
void test_all(Runner runner) {
    test_void_on_error(runner);
    test_typed_on_error(runner);
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    test_void_catch(runner);
    test_typed_catch(runner);
#endif
}
} // namespace

int main() {
    test_all(std::true_type{});
    test_all(std::false_type{});
    std::printf("%s: void and typed recovery contracts (compiler exceptions=%d, strict=%d)\n",
                failures == 0 ? "PASS" : "FAIL", FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS,
                FLUX_FOUNDRY_NO_EXCEPTION_STRICT);
    return failures == 0 ? 0 : 1;
}
