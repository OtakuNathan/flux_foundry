#include "memory/identity_pool.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

using namespace flux_foundry;
namespace {
void check(bool value, const char* reason) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", reason); std::abort(); }
}
using pool = identity_pool_allocator<96, 8, 64, 8, 8, 64>;
static_assert(noexcept(pool::alloc()) && noexcept(pool::dealloc(0, nullptr)) &&
              noexcept(pool::flush_remote_returns()), "pool operations must be noexcept");
static_assert(std::is_integral<pool::identity_type>::value, "identity is a value, not a TLS pointer");

std::vector<void*> drain_backing() {
    std::vector<void*> blocks;
    while (auto p = detail::get_pool().allocate(96)) blocks.push_back(p);
    return blocks;
}
size_t available() {
    auto blocks = drain_backing();
    for (auto p : blocks) detail::get_pool().deallocate(p);
    return blocks.size();
}
void fill(void* p, unsigned char value) {
    check(p != nullptr, "allocation failed");
    std::memset(p, value, 96);
}
void verify(void* p, unsigned char value) {
    auto* bytes = static_cast<unsigned char*>(p);
    for (size_t i = 0; i < 96; ++i) check(bytes[i] == value, "live block reused prematurely");
}
void round_trip() {
    std::thread owner([] {
        const auto identity = pool::current_identity();
        check(identity != 0, "owner registration failed");
        std::vector<void*> original, reused;
        for (int i = 0; i < 61; ++i) {
            auto p = pool::alloc(); fill(p, static_cast<unsigned char>(i)); original.push_back(p);
        }
        std::thread remote([&] {
            for (size_t i = 0; i < original.size(); ++i) {
                verify(original[i], static_cast<unsigned char>(i));
                pool::dealloc(identity, original[i]);
            }
            pool::flush_remote_returns(); // Includes the partial final batch.
        });
        remote.join();
        for (size_t i = 0; i < original.size(); ++i) reused.push_back(pool::alloc());
        for (auto p : reused)
            check(std::find(original.begin(), original.end(), p) != original.end(), "remote block did not return to owner");
        std::sort(reused.begin(), reused.end(), std::less<void*>{});
        check(std::adjacent_find(reused.begin(), reused.end()) == reused.end(), "duplicate live allocation");
        for (auto p : reused) pool::dealloc(identity, p);
        auto p = pool::alloc(); pool::dealloc(identity, p);
        auto again = pool::alloc(); check(p == again, "local LIFO reuse failed"); pool::dealloc(identity, again);
    });
    owner.join();
}

// One slot forces both exhaustion and reuse while old objects still exist.
void stale_identity() {
    using single = identity_pool_allocator<96, 8, 16, 1, 1>;
    void* old = nullptr;
    size_t old_id = 0;
    std::thread first([&] { old_id = single::current_identity(); old = single::alloc(); fill(old, 91); });
    first.join();
    std::atomic<bool> ready{false}, done{false};
    size_t replacement_id = 0;
    std::thread replacement([&] {
        replacement_id = single::current_identity();
        ready.store(true, std::memory_order_release);
        while (!done.load(std::memory_order_acquire)) std::this_thread::yield();
    });
    while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
    check(old_id && replacement_id && old_id != replacement_id, "slot reuse did not advance identity");
    std::thread receiver([&] {
        check(single::current_identity() == 0, "exhausted registry must fall back safely");
        verify(old, 91);
        single::dealloc(old_id, old);
        auto p = single::alloc();
        check(p == old, "stale identity was routed to replacement owner instead of adopted");
        single::dealloc(0, p);
    });
    receiver.join();
    done.store(true, std::memory_order_release);
    replacement.join();
}

void pending_after_exit() {
    void* p = nullptr;
    size_t id = 0;
    std::atomic<bool> allocated{false}, staged{false}, exited{false};
    std::thread owner([&] {
        id = pool::current_identity(); p = pool::alloc(); fill(p, 76);
        allocated.store(true, std::memory_order_release);
        while (!staged.load(std::memory_order_acquire)) std::this_thread::yield();
    });
    std::thread remote([&] {
        while (!allocated.load(std::memory_order_acquire)) std::this_thread::yield();
        verify(p, 76); pool::dealloc(id, p); // Partial batch keeps no admission ticket.
        staged.store(true, std::memory_order_release);
        while (!exited.load(std::memory_order_acquire)) std::this_thread::yield();
        pool::flush_remote_returns();
        auto adopted = pool::alloc();
        check(adopted == p, "late batch was not adopted");
        pool::dealloc(pool::current_identity(), adopted);
    });
    owner.join(); // Must not wait for the partial batch on remote.
    exited.store(true, std::memory_order_release);
    remote.join();
}

void close_race() {
    for (int repeat = 0; repeat < 40; ++repeat) {
        std::vector<void*> blocks(512);
        size_t id = 0;
        std::atomic<bool> ready{false};
        std::atomic<unsigned> started{0};
        std::thread owner([&] {
            id = pool::current_identity();
            for (size_t i = 0; i < blocks.size(); ++i) { blocks[i] = pool::alloc(); fill(blocks[i], i % 251); }
            ready.store(true, std::memory_order_release);
            while (started.load(std::memory_order_acquire) != 4) std::this_thread::yield();
            // Exit races with publication, inbox overflow and sender TLS exit.
        });
        std::vector<std::thread> remotes;
        for (size_t j = 0; j < 4; ++j) remotes.emplace_back([&, j] {
            while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
            started.fetch_add(1, std::memory_order_acq_rel);
            for (size_t i = j; i < blocks.size(); i += 4) {
                verify(blocks[i], i % 251); pool::dealloc(id, blocks[i]);
            }
        });
        owner.join();
        for (auto& t : remotes) t.join();
    }
}

std::atomic<unsigned> late_calls{0};
struct late_destructor {
    void* p = nullptr;
    size_t identity = 0;
    ~late_destructor() {
        check(pool::current_identity() == 0, "TLS cache resurrected after retirement");
        pool::dealloc(identity, p);
        auto* q = pool::alloc(); check(q != nullptr, "late allocation failed");
        pool::dealloc(0, q);
        ++late_calls;
    }
};
void late_tls() {
    std::thread t([] {
        static thread_local late_destructor late; // Destroyed after pool TLS.
        late.p = pool::alloc(); late.identity = pool::current_identity();
    });
    t.join(); check(late_calls == 1, "late TLS destructor did not run");
}
void over_aligned() {
    using aligned = identity_pool_allocator<2048, 256, 16, 4>;
    std::thread t([] {
        auto id = aligned::current_identity();
        auto* p = aligned::alloc();
        check(p && reinterpret_cast<std::uintptr_t>(p) % 256 == 0, "over-aligned allocation failed");
        std::thread other([&] { aligned::dealloc(id, p); });
        other.join();
        auto* again = aligned::alloc(); check(p == again, "sender exit did not flush partial batch");
        aligned::dealloc(id, again);
    });
    t.join();
}
void mixed_routes() {
    using routed = identity_pool_allocator<96, 8, 16, 8>;
    std::atomic<unsigned> ready{0};
    std::atomic<bool> done{false};
    void* pointers[2]{};
    size_t identities[2]{};
    std::thread owners[2];
    for (size_t i = 0; i < 2; ++i) owners[i] = std::thread([&, i] {
        identities[i] = routed::current_identity(); pointers[i] = routed::alloc();
        ready.fetch_add(1, std::memory_order_acq_rel);
        while (!done.load(std::memory_order_acquire)) std::this_thread::yield();
        auto p = routed::alloc();
        check(p == pointers[i], "mixed destinations lost route/partial-batch flush");
        routed::dealloc(identities[i], p);
    });
    while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    std::thread sender([&] {
        routed::dealloc(identities[0], pointers[0]);
        routed::dealloc(identities[1], pointers[1]); // Flushes the first route.
    });
    sender.join(); // Flushes the second route.
    done.store(true, std::memory_order_release);
    for (auto& t : owners) t.join();
}
struct object final : identity_pooling_base<object, 32> {
    unsigned char data[96];
    explicit object(bool fail = false) {
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        if (fail) throw 42;
#else
        (void)fail;
#endif
    }
};
static_assert(object::align() >= alignof(object) && object::alloc_size() >= sizeof(object),
    "identity pooling preserves pooling_base alignment/size helpers");
void object_lifetime() {
    std::thread t([] {
        check(object::operator new(sizeof(object) + 1, std::nothrow) == nullptr,
              "exact-type allocation size check failed");
        auto* p = new object;
        std::thread other([p] { object::destroy_pooled(p); }); other.join();
        delete new object; // Explicit delete safely adopts without reading dead members.
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        bool caught = false;
        try { (void)new object(true); } catch (int) { caught = true; }
        check(caught, "constructor failure path not exercised");
#endif
    });
    t.join();
}
} // namespace

int main() {
    const auto before = available();
    round_trip(); stale_identity(); pending_after_exit(); close_race(); late_tls(); over_aligned(); mixed_routes(); object_lifetime();
    check(available() == before, "pool storage leaked after owner/sender exit");
    auto held = drain_backing();
    round_trip(); stale_identity(); pending_after_exit();
    check(available() == 0, "malloc fallback was misclassified");
    for (auto p : held) detail::get_pool().deallocate(p);
    check(available() == before, "fallback changed backing ownership");
    std::puts("PASS: identity routing, batched TLS refill, stale/exhausted slots, adoption, exit races, late TLS and alignment");
}
