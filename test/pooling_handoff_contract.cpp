#include "memory/pooling.h"
#include "utility/concurrent_queues.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#include <vector>

namespace {
using namespace flux_foundry;
void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::abort(); }
}
struct packet { std::uint64_t sequence, words[7]; };

std::vector<void*> drain_shared_pool() {
    std::vector<void*> blocks;
    while (auto p = detail::get_pool().allocate(sizeof(packet))) blocks.push_back(p);
    return blocks;
}
void return_blocks(std::vector<void*>& blocks) {
    for (auto p : blocks) detail::get_pool().deallocate(p);
    blocks.clear();
}
std::size_t shared_available() {
    auto blocks = drain_shared_pool();
    const auto count = blocks.size();
    return_blocks(blocks);
    return count;
}

template <std::size_t Cache>
void handoff() {
    using allocator = flux_foundry_allocator<sizeof(packet), alignof(packet), Cache>;
    static_assert(noexcept(std::declval<allocator&>().alloc()) &&
                  noexcept(std::declval<allocator&>().dealloc(nullptr)),
                  "pool allocator operations must remain noexcept");
    // Stack placement preserves the queue's cache-line alignment in C++14.
    spsc_queue<void*, 64> queue;
    constexpr std::uint64_t count = 4096;
    std::thread producer([&] {
        allocator a;
        for (std::uint64_t i = 0; i < count; ++i) {
            void* memory = a.alloc();
            check(memory != nullptr, "allocation failed");
            check(reinterpret_cast<std::uintptr_t>(memory) % alignof(packet) == 0,
                  "incorrect payload alignment");
            auto* p = new (memory) packet{};
            p->sequence = i;
            for (std::size_t j = 0; j < 7; ++j) p->words[j] = i ^ (UINT64_C(0xabcdef0123456789) + j);
            queue.wait_and_emplace(std::move(memory));
        }
    });
    std::thread consumer([&] {
        allocator a;
        for (std::uint64_t i = 0; i < count; ++i) {
            auto* p = static_cast<packet*>(queue.wait_and_pop());
            check(p->sequence == i, "live payload was aliased or reordered");
            for (std::size_t j = 0; j < 7; ++j)
                check(p->words[j] == (i ^ (UINT64_C(0xabcdef0123456789) + j)),
                      "payload changed before ownership transfer completed");
            p->~packet();
            a.dealloc(p);
        }
    });
    producer.join();
    consumer.join();
    check(!queue.try_pop().has_value(), "handoff queue did not drain");
}

void all_capacities() {
    handoff<1>(); handoff<2>(); handoff<3>(); handoff<4>();
    handoff<16>(); handoff<512>();
}
} // namespace

int main() {
    const auto available = shared_available();
    check(available > 64, "shared pool unexpectedly empty");
    all_capacities();
    check(shared_available() == available, "thread exit leaked shared-pool blocks");

    // Exhaust the shared pool to exercise malloc-backed cache entries.
    auto held = drain_shared_pool();
    all_capacities();
    check(shared_available() == 0, "fallback entries were misclassified as pool blocks");
    return_blocks(held);
    check(shared_available() == available, "fallback handoff changed pool ownership");

    // Leave a few shared blocks available, so caches contain both origins.
    held = drain_shared_pool();
    for (int i = 0; i < 8; ++i) {
        detail::get_pool().deallocate(held.back());
        held.pop_back();
    }
    all_capacities();
    check(shared_available() == 8, "mixed-origin cache lost or duplicated pool blocks");
    return_blocks(held);
    check(shared_available() == available, "mixed-origin handoff did not fully reclaim");
    std::puts("PASS: cross-thread pooling, small/full caches, fallback/mixed origins and TLS exit reclamation");
}
