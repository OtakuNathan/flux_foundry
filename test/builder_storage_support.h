#ifndef FLUX_FOUNDRY_TEST_BUILDER_STORAGE_SUPPORT_H
#define FLUX_FOUNDRY_TEST_BUILDER_STORAGE_SUPPORT_H

#include "flow/flow.h"
#include <cstdlib>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace builder_storage_test {
namespace ff = flux_foundry;
using result = ff::result_t<int, ff::flow_async_agg_err_t>;

struct counts {
    int copies = 0;
    int moves = 0;
};

// A non-empty state exercises the non-EBO storage path. Potentially throwing
// operations abort if reached, so accepted rvalues also prove no copy occurred.
template <bool CopyNoexcept, bool MoveNoexcept>
struct state {
    counts* observed;
    explicit state(counts* c = nullptr) noexcept : observed(c) {}
    state(const state& other) noexcept(CopyNoexcept) : observed(other.observed) {
        if (!CopyNoexcept) std::abort();
        if (observed) ++observed->copies;
    }
    state(state&& other) noexcept(MoveNoexcept) : observed(other.observed) {
        if (!MoveNoexcept) std::abort();
        if (observed) ++observed->moves;
    }
};

template <bool C, bool M>
struct calc : state<C, M> {
    using state<C, M>::state;
    int operator()(int n) noexcept { return n + 1; }
};

template <bool C, bool M>
struct result_callback : state<C, M> {
    using state<C, M>::state;
    result operator()(result&& in) noexcept { return std::move(in); }
};

template <bool C, bool M>
struct catch_callback : state<C, M> {
    using state<C, M>::state;
    int operator()(const std::exception&) noexcept { return 7; }
};

template <bool C, bool M>
struct success : state<C, M> {
    using state<C, M>::state;
    result operator()(int n) const noexcept { return result(ff::value_tag, n); }
    result operator()(std::size_t, int n) const noexcept { return result(ff::value_tag, n); }
};

template <bool C, bool M>
struct failure : state<C, M> {
    using state<C, M>::state;
    result operator()(ff::flow_async_agg_err_t e) const noexcept { return result(ff::error_tag, e); }
};

struct executor {
    void dispatch(ff::task_wrapper_sbo&& task) noexcept { task(); }
};

template <bool C, bool M>
struct handle : state<C, M> {
    using state<C, M>::state;
    executor* operator->() const noexcept {
        static executor e;
        return &e;
    }
};

struct awaitable final : ff::awaitable_base<awaitable, int, ff::flow_async_agg_err_t> {
    using async_result_type = result;
    explicit awaitable(result&&) noexcept {}
    int submit() noexcept { return 0; }
    bool available() noexcept { return true; }
    void cancel() noexcept {}
};

using branch = decltype(ff::make_blueprint<int, ff::flow_async_agg_err_t>() | ff::end());

struct receiver {
    using value_type = result;
    int* output;
    void emplace(result&& in) noexcept { *output = in.has_value() ? in.value() : -1; }
};

} // namespace builder_storage_test
#endif
