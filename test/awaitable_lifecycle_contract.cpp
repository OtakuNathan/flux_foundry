#include "flow/flow.h"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

using namespace flux_foundry;
using output = result_t<int, std::exception_ptr>;
enum class completion { inline_now, worker_before_return, delayed, submit_failure };
struct observation {
    completion mode;
    std::atomic<int> created{0}, destroyed{0}, submitted{0}, canceled{0};
    std::atomic<int> delivered{0}, values{0}, errors{0};
    std::function<void()> complete;
    explicit observation(completion m) : mode(m) {}
};
struct controlled_awaitable final : awaitable_base<controlled_awaitable, int, std::exception_ptr> {
    using async_result_type = output;
    observation* state;
    explicit controlled_awaitable(result_t<observation*, std::exception_ptr>&& in) noexcept
        : state(in.value()) { ++state->created; }
    ~controlled_awaitable() { ++state->destroyed; }
    int submit() noexcept {
        ++state->submitted;
        if (state->mode == completion::submit_failure) return -1;
        if (state->mode == completion::inline_now) {
            resume(output(value_tag, 42));
        } else {
            retain(); // Backend keeps the object alive even if cancellation wins.
            auto callback = [this]() noexcept {
                resume(output(value_tag, 42));
                release();
            };
            if (state->mode == completion::worker_before_return) {
                std::thread worker(callback);
                worker.join();
            } else {
                state->complete = callback;
            }
        }
        return 0;
    }
    void cancel() noexcept { ++state->canceled; }
};
struct receiver {
    using value_type = output;
    observation* state;
    void emplace(output&& out) noexcept {
        ++state->delivered;
        if (out.has_value() && out.value() == 42) ++state->values;
        else if (out.has_error()) ++state->errors;
    }
};
// Drain explicitly to separate backend completion from receiver execution.
struct manual_executor {
    std::vector<task_wrapper_sbo> tasks;
    void dispatch(task_wrapper_sbo&& task) noexcept { tasks.push_back(std::move(task)); }
    void drain() { auto pending = std::move(tasks); for (auto& task : pending) task(); }
};

template <typename Runner>
bool exercise(Runner& runner, observation& state, const lite_ptr<flow_controller>& control,
              bool cancel_first, bool pre_cancel) {
    if (pre_cancel) control->cancel();
    runner(&state);
    bool ok = true;
    if (state.mode == completion::delayed && !pre_cancel) {
        // No sleeps: keep backend completion behind a gate until the selected
        // cancellation point has finished, then join before inspecting counts.
        std::mutex mutex;
        std::condition_variable cv;
        bool allowed = false;
        std::thread worker([&] {
            { std::unique_lock<std::mutex> lock(mutex); cv.wait(lock, [&] { return allowed; }); }
            state.complete();
        });
        if (cancel_first) {
            control->cancel();
            ok = state.destroyed == 0; // Receiver completion must not free the backend object.
        }
        { std::lock_guard<std::mutex> lock(mutex); allowed = true; }
        cv.notify_one();
        worker.join();
        state.complete = nullptr;
        if (!cancel_first) control->cancel();
    }
    return ok;
}

bool run_case(completion mode, bool cancel_first, bool pre_cancel, bool queued) {
    observation state(mode);
    bool ok;
    auto control = make_lite_ptr<flow_controller>();
    if (queued) {
        manual_executor executor;
        auto bp = make_blueprint<observation*>() | await<controlled_awaitable>(&executor) | end();
        flow_runner<decltype(bp), receiver> runner(
            make_lite_ptr<decltype(bp)>(std::move(bp)), control, receiver{&state});
        ok = exercise(runner, state, control, cancel_first, pre_cancel);
        ok = ok && state.delivered == 0;
        executor.drain();
    } else {
        auto bp = make_blueprint<observation*>() | await<controlled_awaitable>() | end();
        flow_runner<decltype(bp), receiver> runner(
            make_lite_ptr<decltype(bp)>(std::move(bp)), control, receiver{&state});
        ok = exercise(runner, state, control, cancel_first, pre_cancel);
    }
    const bool error = pre_cancel || cancel_first || mode == completion::submit_failure;
    ok = ok && state.delivered == 1 && state.errors == (error ? 1 : 0)
        && state.values == (error ? 0 : 1) && state.created == (pre_cancel ? 0 : 1)
        && state.destroyed == state.created && state.submitted == (pre_cancel ? 0 : 1)
        && state.canceled == (cancel_first && !pre_cancel ? 1 : 0);
    std::printf("[%s] lifecycle mode=%d cancel-first=%d pre-cancel=%d queued=%d\n",
        ok ? "OK" : "FAIL", static_cast<int>(mode), cancel_first, pre_cancel, queued);
    return ok;
}
int main() {
    bool ok = true;
    for (bool queued : {false, true}) {
        for (auto mode : {completion::inline_now, completion::worker_before_return,
                          completion::delayed, completion::submit_failure}) {
            ok = run_case(mode, false, false, queued) && ok;
        }
        ok = run_case(completion::delayed, true, false, queued) && ok;
        ok = run_case(completion::delayed, false, true, queued) && ok;
    }
    return ok ? 0 : 1;
}
