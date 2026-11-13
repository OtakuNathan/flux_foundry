#include <uv.h>
#include <atomic>
static int observed_async_send(uv_async_t*) noexcept;
// Test seam around the native send: pause AFTER it wakes the loop but BEFORE
// the adapter may drop its ticket. Production headers need no test hooks.
#define uv_async_send observed_async_send
#include "executor/uv_executor.h"
#undef uv_async_send
#include "base/traits.h"
#include "flow/flow_def.h"
#include <system_error>
#if !FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
namespace flux_foundry {
template<> struct cancel_error<std::error_code> {static std::error_code make(cancel_kind) noexcept {return std::make_error_code(std::errc::operation_canceled);}};
template<> struct awaitable_creating_error<std::error_code> {static std::error_code make() noexcept {return std::make_error_code(std::errc::not_enough_memory);}};
template<> struct async_submission_failed_error<std::error_code> {static std::error_code make() noexcept {return std::make_error_code(std::errc::resource_unavailable_try_again);}};
}
using error_type=std::error_code;
#else
using error_type=std::exception_ptr;
#endif
#include "flow/flow.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <thread>
#include <vector>

#if !FLUX_FOUNDRY_WITH_TSAN
// TSan supplies strong new/delete interceptors. Fault injection is exercised
// by the regular and UBSan builds; the TSan build focuses on synchronization.
static std::atomic<bool> fail_allocations{false};
#if defined(__GNUC__) || defined(__clang__)
#define FF_TEST_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define FF_TEST_NOINLINE __declspec(noinline)
#else
#define FF_TEST_NOINLINE
#endif
// Keep the replacement pair opaque to the optimizer's mismatched-new/delete
// heuristic; both sides deliberately use malloc/free for fault injection.
FF_TEST_NOINLINE
void* operator new(std::size_t size) {
    if (fail_allocations.load()) {
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        throw std::bad_alloc();
#else
        std::abort(); // No queue allocation is permitted during this interval.
#endif
    }
    if (void* p = std::malloc(size ? size : 1)) return p;
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    throw std::bad_alloc();
#else
    std::abort();
#endif
}
FF_TEST_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
FF_TEST_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#endif

using namespace flux_foundry;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::abort(); } } while (false)

static std::atomic<bool> pause_send{false}, send_entered{false}, release_send{false};
static int observed_async_send(uv_async_t* handle) noexcept {
    const int rc = uv_async_send(handle);
    if (pause_send.exchange(false, std::memory_order_acq_rel)) {
        send_entered.store(true, std::memory_order_release);
        while (!release_send.load(std::memory_order_acquire)) std::this_thread::yield();
    }
    return rc;
}

void outstanding_send(bool closer) {
    uv_loop_t loop{}; CHECK(uv_loop_init(&loop) == 0);
    uv_executor ex(&loop, 1, 1);
    int calls = 0;
    send_entered = false; release_send = false; pause_send = true;
    std::thread sender([&] {
        if (closer) ex.close();
        else CHECK(ex.try_dispatch(task_wrapper_sbo([&]() noexcept { ++calls; })));
    });
    while (!send_entered.load(std::memory_order_acquire)) std::this_thread::yield();
    ex.close(); // Idempotent; cannot bypass the paused sender's ticket.
    for (int i = 0; i < 4; ++i) uv_run(&loop, UV_RUN_NOWAIT);
    CHECK(calls == (closer ? 0 : 1) && !ex.closed());
    // The task is already drained, but native send has not returned to FF.
    release_send.store(true, std::memory_order_release);
    sender.join();
    CHECK(uv_run(&loop, UV_RUN_DEFAULT) == 0 && ex.closed());
    CHECK(uv_loop_close(&loop) == 0);
}

struct publication_barrier {
    std::atomic<bool> pause{false}, entered{false}, release{false};
    int calls = 0;
};
struct delayed_move {
    publication_barrier* barrier;
    explicit delayed_move(publication_barrier* p) noexcept : barrier(p) {}
    delayed_move(delayed_move&& other) noexcept : barrier(other.barrier) {
        if (barrier->pause.exchange(false, std::memory_order_acq_rel)) {
            barrier->entered.store(true, std::memory_order_release);
            while (!barrier->release.load(std::memory_order_acquire)) std::this_thread::yield();
        }
    }
    void operator()() noexcept { CHECK(barrier->calls == 0); ++barrier->calls; }
};
void unpublished_reservation() {
    static_assert(sizeof(delayed_move) <= task_wrapper_sbo::sbo_size, "must move inside queue publication");
    uv_loop_t loop{}; CHECK(uv_loop_init(&loop) == 0);
    uv_executor ex(&loop, 3, 1); // Arbitrary capacities, including non-powers of two.
    publication_barrier barrier;
    std::thread producer([&] {
        task_wrapper_sbo task(delayed_move{&barrier});
        barrier.pause = true;
        CHECK(ex.try_dispatch(std::move(task)));
    });
    while (!barrier.entered.load(std::memory_order_acquire)) std::this_thread::yield();
    CHECK(ex.try_dispatch(task_wrapper_sbo([&]() noexcept { CHECK(barrier.calls == 1); ++barrier.calls; })));
    ex.close();
    for (int i = 0; i < 4; ++i) uv_run(&loop, UV_RUN_NOWAIT);
    CHECK(!ex.closed() && !barrier.calls); // Neither bypass nor discard the hole.
    barrier.release.store(true, std::memory_order_release);
    producer.join();
    CHECK(uv_run(&loop, UV_RUN_DEFAULT) == 0 && ex.closed() && barrier.calls == 2);
    CHECK(uv_loop_close(&loop) == 0);
}

void bounded_drain(std::size_t budget) {
    uv_loop_t loop{}; CHECK(uv_loop_init(&loop) == 0);
    uv_executor ex(&loop, 32, budget);
    const auto owner = std::this_thread::get_id();
    int count = 0, rejected = 0;
    ex.dispatch(task_wrapper_sbo([&]() noexcept {
        ex.dispatch(task_wrapper_sbo([&]() noexcept { ++count; }));
    }));
    CHECK(count == 1); // Owner + empty queue is an inline path.
    std::thread producer([&] {
        for (int i = 0; i < 32; ++i)
            CHECK(ex.try_dispatch(task_wrapper_sbo([&, i]() noexcept {
                CHECK(std::this_thread::get_id() == owner);
                CHECK(count == i + 1); ++count;
            })));
        task_wrapper_sbo last([&]() noexcept { ++rejected; });
        CHECK(!ex.try_dispatch(std::move(last)) && last);
        last();
        ex.close(); ex.close();
        CHECK(!ex.try_dispatch(task_wrapper_sbo([]() noexcept {})));
    });
    producer.join();
    CHECK(!ex.closed() && ex.loop() == nullptr);
    CHECK(uv_run(&loop, UV_RUN_DEFAULT) == 0);
    CHECK(ex.closed() && count == 33 && rejected == 1);
    ex.close(); // Must not send on a closed handle.
    CHECK(uv_loop_close(&loop) == 0);
}

void owner_backpressure() {
    uv_loop_t loop{}; CHECK(uv_loop_init(&loop) == 0);
    uv_executor ex(&loop, 1, 1);
    int calls = 0;
    std::thread producer([&] { ex.dispatch(task_wrapper_sbo([&]() noexcept { ++calls; })); });
    producer.join();
    task_wrapper_sbo retained([&]() noexcept { ++calls; });
    // Even the owner does not spin waiting for its own full queue.
    CHECK(!ex.try_dispatch(std::move(retained)) && retained);
    ex.close(); uv_run(&loop, UV_RUN_DEFAULT);
    CHECK(calls == 1); retained(); CHECK(calls == 2);
    CHECK(uv_loop_close(&loop) == 0);
}

void independent_lifetime(bool inside_callback) {
    uv_loop_t loop{}; CHECK(uv_loop_init(&loop) == 0);
    std::unique_ptr<uv_executor> ex(new uv_executor(&loop, 8, 1));
    int calls = 0;
    std::thread producer([&] {
        ex->dispatch(task_wrapper_sbo([&]() noexcept {
            ++calls; if (inside_callback) ex.reset();
        }));
        ex->dispatch(task_wrapper_sbo([&]() noexcept { ++calls; }));
    });
    producer.join();
    if (!inside_callback) ex.reset();
    CHECK(uv_run(&loop, UV_RUN_DEFAULT) == 0);
    CHECK(!ex && calls == 2 && uv_loop_close(&loop) == 0);
}

void inline_lifetime(bool nested) {
    uv_loop_t loop{}; CHECK(uv_loop_init(&loop) == 0);
    std::unique_ptr<uv_executor> ex(new uv_executor(&loop, 8, 1));
    int calls = 0;
    // Destroy the public object while try_dispatch still owns an admission
    // ticket. A nested inline submission leaves two active dispatch frames.
    ex->dispatch(task_wrapper_sbo([&]() noexcept {
        ++calls;
        if (nested) {
            ex->dispatch(task_wrapper_sbo([&]() noexcept { ++calls; ex.reset(); }));
        } else {
            ex.reset();
        }
    }));
    CHECK(!ex && calls == (nested ? 2 : 1));
    CHECK(uv_run(&loop, UV_RUN_DEFAULT) == 0);
    CHECK(uv_loop_close(&loop) == 0);
}

void allocation_failure() {
#if FLUX_FOUNDRY_WITH_TSAN
    std::puts("SKIP: allocation replacement under TSan (covered by regular build)");
#else
    uv_loop_t loop{}; CHECK(uv_loop_init(&loop) == 0);
    uv_executor ex(&loop, 1024, 3);
    int accepted = 0, executed = 0;
    std::thread producer([&] {
        fail_allocations = true;
        for (;;) {
            task_wrapper_sbo task([&]() noexcept { ++executed; });
            if (!ex.try_dispatch(std::move(task))) { CHECK(task); break; }
            ++accepted;
        }
        fail_allocations = false;
        CHECK(accepted == 1024); // Preallocated ring: only capacity rejects; no allocation.
        ex.close();
    });
    producer.join();
    CHECK(uv_run(&loop, UV_RUN_DEFAULT) == 0);
    CHECK(executed == accepted && uv_loop_close(&loop) == 0);
#endif
}

void race_close() {
    // Multiple producers keep using the live executor after close is requested
    // and after its loop exits. No async_send may race uv_close/deallocation.
    for (int iteration = 0; iteration < 40; ++iteration) {
        uv_loop_t loop{}; CHECK(uv_loop_init(&loop) == 0);
        uv_executor ex(&loop, 64, 3);
        std::atomic<int> accepted{0}, executed{0}, attempts{0};
        std::atomic<bool> go{false};
        std::vector<std::thread> producers;
        for (int p = 0; p < 4; ++p) producers.emplace_back([&] {
            while (!go.load()) std::this_thread::yield();
            for (int i = 0; i < 1000; ++i) {
                task_wrapper_sbo task([&]() noexcept { ++executed; });
                if (ex.try_dispatch(std::move(task))) ++accepted;
                else CHECK(task);
                ++attempts;
            }
        });
        std::thread closer([&] {
            while (attempts.load() < 100) std::this_thread::yield();
            ex.close(); ex.close();
        });
        go = true;
        CHECK(uv_run(&loop, UV_RUN_DEFAULT) == 0);
        for (auto& producer : producers) producer.join();
        closer.join();
        CHECK(ex.closed() && accepted > 0 && accepted == executed);
        CHECK(uv_loop_close(&loop) == 0);
    }
}

void concurrent_waves(std::size_t budget, std::size_t capacity = 64) {
    uv_loop_t loop{}; CHECK(uv_loop_init(&loop) == 0);
    uv_executor ex(&loop, capacity, budget);
    constexpr int producers = 4, waves = 100, per_wave = 31;
    std::atomic<int> delivered{0};
    int expected[producers]{};
    std::vector<std::thread> workers;
    for (int p = 0; p < producers; ++p) workers.emplace_back([&, p] {
        for (int wave = 0; wave < waves; ++wave) {
            for (int i = 0; i < per_wave; ++i) {
                const int value = wave * per_wave + i;
                task_wrapper_sbo task([&, p, value]() noexcept {
                    CHECK(expected[p]++ == value);
                    delivered.fetch_add(1, std::memory_order_release);
                });
                while (!ex.try_dispatch(std::move(task))) {
                    CHECK(task);
                    std::this_thread::yield();
                }
            }
            while (delivered.load(std::memory_order_acquire) < (wave + 1) * per_wave * producers)
                std::this_thread::yield();
        }
    });
    std::thread closer([&] {
        for (auto& worker : workers) worker.join();
        ex.close();
    });
    CHECK(uv_run(&loop, UV_RUN_DEFAULT) == 0);
    closer.join();
    for (int count : expected) CHECK(count == waves * per_wave);
    CHECK(ex.closed() && uv_loop_close(&loop) == 0);
}

struct receiver {
    using value_type = result_t<int, error_type>;
    int* output;
    void emplace(value_type&& r) noexcept { CHECK(r.has_value()); *output = r.value(); }
};
void runners() {
    uv_loop_t loop{}; CHECK(uv_loop_init(&loop) == 0);
    uv_executor ex(&loop);
    const auto owner = std::this_thread::get_id();
    auto bp = make_blueprint<int,error_type>() | via(&ex)
        | transform([&](int v) noexcept { CHECK(std::this_thread::get_id() == owner); return v + 2; }) | end();
    auto shared = make_lite_ptr<decltype(bp)>(std::move(bp));
    int normal_value = 0, fast_value = 0;
    auto normal = make_runner(shared, receiver{&normal_value});
    auto fast = make_fast_runner(shared, receiver{&fast_value});
    std::thread producer([&] { normal(40); fast(41); ex.close(); });
    producer.join();
    CHECK(uv_run(&loop, UV_RUN_DEFAULT) == 0);
    CHECK(normal_value == 42 && fast_value == 43);
    CHECK(uv_loop_close(&loop) == 0);
}

int main() {
    uv_executor invalid(std::nothrow,nullptr);
    CHECK(invalid.error()==UV_EINVAL && invalid.closed() && !invalid.loop());
    task_wrapper_sbo retained([]() noexcept {});
    CHECK(!invalid.try_dispatch(std::move(retained)) && retained);
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
    bool failed=false;
    try {uv_executor legacy(nullptr);} catch(const std::invalid_argument&) {failed=true;}
    CHECK(failed);
#endif
    for (auto budget : {1u, 3u, 16u, 64u}) bounded_drain(budget);
    owner_backpressure();
    independent_lifetime(false); independent_lifetime(true);
    inline_lifetime(false); inline_lifetime(true);
    allocation_failure(); race_close(); runners();
    outstanding_send(false); outstanding_send(true); unpublished_reservation();
    for (auto budget : {1u, 3u, 16u, 64u}) concurrent_waves(budget);
    for (auto capacity : {1u, 2u, 3u, 7u}) concurrent_waves(3, capacity);
    std::puts("PASS: libuv bounds, retained rejection, owner affinity/inline, coalesced wakeups, budget rearm, close drain/races, independent handle lifetime, normal/fast runners");
}
