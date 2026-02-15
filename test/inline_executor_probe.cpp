#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "flow/flow.h"

using namespace flux_foundry;

namespace {
using err_t = std::exception_ptr;
using out_t = result_t<int, err_t>;

struct inline_executor {
    void dispatch(task_wrapper_sbo t) noexcept {
        t();
    }
};

class work_group {
public:
    explicit work_group(size_t nthreads)
        : stopping_(false) {
        workers_.reserve(nthreads);
        for (size_t i = 0; i < nthreads; ++i) {
            workers_.emplace_back([this]() { this->run(); });
        }
    }

    ~work_group() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) {
                w.join();
            }
        }
    }

    void post(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lk(m_);
            q_.push_back(std::move(fn));
        }
        cv_.notify_one();
    }

private:
    void run() {
        for (;;) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this]() { return stopping_ || !q_.empty(); });
                if (stopping_ && q_.empty()) {
                    return;
                }
                fn = std::move(q_.front());
                q_.pop_front();
            }
            fn();
        }
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    std::vector<std::thread> workers_;
    bool stopping_;
};

work_group& backend_pool() {
    static work_group p(4);
    return p;
}

void spin_for_us(unsigned us) {
    auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    while (std::chrono::steady_clock::now() < until) {
    }
}

struct immediate_plus_one_awaitable final : awaitable_base<immediate_plus_one_awaitable, int, err_t> {
    using async_result_type = out_t;
    int v;

    explicit immediate_plus_one_awaitable(async_result_type&& in) noexcept
        : v(in.has_value() ? in.value() : 0) {
    }

    int submit() noexcept {
        this->resume(async_result_type(value_tag, v + 1));
        return 0;
    }

    void cancel() noexcept {
    }
};

struct delayed_plus_one_awaitable final : awaitable_base<delayed_plus_one_awaitable, int, err_t> {
    using async_result_type = out_t;
    int v;

    explicit delayed_plus_one_awaitable(async_result_type&& in) noexcept
        : v(in.has_value() ? in.value() : 0) {
    }

    int submit() noexcept {
        this->retain();
        int x = v;
        backend_pool().post([self = this, x]() noexcept {
            spin_for_us(50);
            self->resume(async_result_type(value_tag, x + 1));
            self->release();
        });
        return 0;
    }

    void cancel() noexcept {
    }
};

struct fail_submit_awaitable final : awaitable_base<fail_submit_awaitable, int, err_t> {
    using async_result_type = out_t;

    explicit fail_submit_awaitable(async_result_type&&) noexcept {
    }

    int submit() noexcept {
        return -1;
    }

    void cancel() noexcept {
    }
};

struct recv_state {
    std::atomic<int> done{0};
    int value{0};
    std::exception_ptr err;
};

struct receiver {
    using value_type = out_t;
    std::shared_ptr<recv_state> st;

    void emplace(value_type&& r) noexcept {
        if (r.has_value()) {
            st->value = r.value();
        } else {
            st->err = r.error();
        }
        st->done.fetch_add(1, std::memory_order_release);
    }
};

bool wait_done(const std::shared_ptr<recv_state>& st, int timeout_ms) {
    auto begin = std::chrono::steady_clock::now();
    while (st->done.load(std::memory_order_acquire) == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        auto elapsed = std::chrono::steady_clock::now() - begin;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > timeout_ms) {
            return false;
        }
    }
    return true;
}

bool is_logic_error(const std::exception_ptr& ep, const char* msg) {
    if (!ep) {
        return false;
    }
    try {
        std::rethrow_exception(ep);
    } catch (const std::logic_error& e) {
        return std::string(e.what()) == msg;
    } catch (...) {
        return false;
    }
}

struct test_stat {
    const char* name;
    int iters{0};
    int timeouts{0};
    int duplicates{0};
    int bad_values{0};
    int bad_errors{0};
};

void print_stat(const test_stat& s) {
    std::printf("[%s] iters=%d timeout=%d dup=%d bad_value=%d bad_error=%d\n",
        s.name, s.iters, s.timeouts, s.duplicates, s.bad_values, s.bad_errors);
}

test_stat test_inline_async_chain() {
    test_stat s{"inline_async_chain", 200000};
    inline_executor ex;

    auto bp = make_blueprint<int>()
        | await<immediate_plus_one_awaitable>(&ex)
        | await<immediate_plus_one_awaitable>(&ex)
        | await<immediate_plus_one_awaitable>(&ex)
        | end();

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));

    for (int i = 0; i < s.iters; ++i) {
        auto st = std::make_shared<recv_state>();
        auto r = make_runner(bp_ptr, receiver{st});
        r(i);

        int done = st->done.load(std::memory_order_acquire);
        if (done == 0) {
            ++s.timeouts;
            continue;
        }
        if (done != 1) {
            ++s.duplicates;
        }
        if (st->err || st->value != i + 3) {
            ++s.bad_values;
        }
    }
    return s;
}

test_stat test_inline_when_all() {
    test_stat s{"inline_when_all", 120000};
    inline_executor ex;

    auto leaf1 = make_blueprint<int>() | transform([](int x) noexcept { return x + 10; }) | end();
    auto leaf2 = make_blueprint<int>() | transform([](int x) noexcept { return x + 20; }) | end();

    auto p1 = make_lite_ptr<decltype(leaf1)>(std::move(leaf1));
    auto p2 = make_lite_ptr<decltype(leaf2)>(std::move(leaf2));

    auto bp = await_when_all(
        &ex,
        [](int a, int b) noexcept { return out_t(value_tag, a + b); },
        [](flow_async_agg_err_t e) noexcept { return out_t(error_tag, std::move(e)); },
        p1, p2)
        | end();

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));

    for (int i = 0; i < s.iters; ++i) {
        auto st = std::make_shared<recv_state>();
        auto r = make_runner(bp_ptr, receiver{st});
        r(i, i + 1);

        int done = st->done.load(std::memory_order_acquire);
        if (done == 0) {
            ++s.timeouts;
            continue;
        }
        if (done != 1) {
            ++s.duplicates;
        }
        if (st->err || st->value != (i + 10) + (i + 1 + 20)) {
            ++s.bad_values;
        }
    }

    return s;
}

test_stat test_inline_when_any() {
    test_stat s{"inline_when_any", 120000};
    inline_executor ex;

    auto leaf1 = make_blueprint<int>() | transform([](int x) noexcept { return x + 100; }) | end();
    auto leaf2 = make_blueprint<int>() | transform([](int x) noexcept { return x + 200; }) | end();

    auto p1 = make_lite_ptr<decltype(leaf1)>(std::move(leaf1));
    auto p2 = make_lite_ptr<decltype(leaf2)>(std::move(leaf2));

    auto bp = await_when_any(
        &ex,
        [](int v) noexcept { return out_t(value_tag, v); },
        [](flow_async_agg_err_t e) noexcept { return out_t(error_tag, std::move(e)); },
        p1, p2)
        | end();

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));

    for (int i = 0; i < s.iters; ++i) {
        auto st = std::make_shared<recv_state>();
        auto r = make_runner(bp_ptr, receiver{st});
        r(i, i + 1);

        int done = st->done.load(std::memory_order_acquire);
        if (done == 0) {
            ++s.timeouts;
            continue;
        }
        if (done != 1) {
            ++s.duplicates;
        }
        int v1 = i + 100;
        int v2 = i + 1 + 200;
        if (st->err || (st->value != v1 && st->value != v2)) {
            ++s.bad_values;
        }
    }

    return s;
}

test_stat test_inline_submit_fail() {
    test_stat s{"inline_submit_fail", 200000};
    inline_executor ex;

    auto bp = make_blueprint<int>()
        | await<fail_submit_awaitable>(&ex)
        | end();

    auto bp_ptr = make_lite_ptr<decltype(bp)>(std::move(bp));

    for (int i = 0; i < s.iters; ++i) {
        auto st = std::make_shared<recv_state>();
        auto r = make_runner(bp_ptr, receiver{st});
        r(i);

        int done = st->done.load(std::memory_order_acquire);
        if (done == 0) {
            ++s.timeouts;
            continue;
        }
        if (done != 1) {
            ++s.duplicates;
        }
        if (!is_logic_error(st->err, "failed to submit async operation")) {
            ++s.bad_errors;
        }
    }

    return s;
}

test_stat test_inline_cancel_after_start() {
    test_stat s{"inline_cancel_after_start", 4000};
    inline_executor ex;

    auto bp = make_blueprint<int>()
        | await<delayed_plus_one_awaitable>(&ex)
        | end();
    using bp_t = decltype(bp);
    auto bp_ptr = make_lite_ptr<bp_t>(std::move(bp));

    for (int i = 0; i < s.iters; ++i) {
        auto st = std::make_shared<recv_state>();
        auto ctrl = make_lite_ptr<flow_controller>();
        flow_runner<bp_t, receiver> r(bp_ptr, ctrl, receiver{st});

        r(i);
        ctrl->cancel((i & 1) != 0);

        if (!wait_done(st, 500)) {
            ++s.timeouts;
            continue;
        }

        int done = st->done.load(std::memory_order_acquire);
        if (done != 1) {
            ++s.duplicates;
        }

        if (st->err) {
            if (!is_logic_error(st->err, "flow soft-canceled") &&
                !is_logic_error(st->err, "flow hard-canceled")) {
                ++s.bad_errors;
            }
        }
    }

    return s;
}

int failures(const test_stat& s) {
    return s.timeouts + s.duplicates + s.bad_values + s.bad_errors;
}

} // namespace

int main() {
    auto t1 = test_inline_async_chain();
    auto t2 = test_inline_when_all();
    auto t3 = test_inline_when_any();
    auto t4 = test_inline_submit_fail();
    auto t5 = test_inline_cancel_after_start();

    print_stat(t1);
    print_stat(t2);
    print_stat(t3);
    print_stat(t4);
    print_stat(t5);

    int failed = failures(t1) + failures(t2) + failures(t3) + failures(t4) + failures(t5);
    if (failed == 0) {
        std::printf("[PASS] inline executor valid-use probe passed\n");
        return 0;
    }

    std::printf("[FAIL] inline executor valid-use probe found %d issues\n", failed);
    return 1;
}
