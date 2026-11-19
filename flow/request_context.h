#ifndef FLUX_FOUNDRY_REQUEST_CONTEXT_H
#define FLUX_FOUNDRY_REQUEST_CONTEXT_H

#include <cassert>
#include <type_traits>
#include <utility>

namespace flux_foundry {

template <typename Context>
class contextual_error;

template <typename Payload>
class request_context {
public:
    using error_type = contextual_error<request_context>;

    constexpr request_context() = default;

    template <typename Value, std::enable_if_t<
        !std::is_same<std::decay_t<Value>, request_context>::value &&
        std::is_constructible<Payload, Value&&>::value, int> = 0>
    explicit constexpr request_context(Value&& payload)
        noexcept(std::is_nothrow_constructible<Payload, Value&&>::value)
        : payload_(std::forward<Value>(payload)) {}

    constexpr Payload& payload() & noexcept { return payload_; }
    constexpr const Payload& payload() const & noexcept { return payload_; }

    error_type fail(int submit_code) & noexcept {
        submit_code_ = submit_code;
        return error_type(this);
    }

private:
    friend class contextual_error<request_context>;
    Payload payload_{};
    int submit_code_ = 0;
};

// The request owns the status slot and must outlive every handle that uses it.
template <typename Context>
class contextual_error {
public:
    constexpr contextual_error() noexcept = default;

    constexpr explicit operator bool() const noexcept { return context_ != nullptr; }
    constexpr Context* context() const noexcept { return context_; }

    int submit_code() const noexcept {
        assert(context_ && "submit_code requires a request context");
        return context_->submit_code_;
    }

private:
    friend Context;
    explicit constexpr contextual_error(Context* context) noexcept : context_(context) {}

    Context* context_ = nullptr;
};

} // namespace flux_foundry

#endif
