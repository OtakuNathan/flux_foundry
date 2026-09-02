#include "../memory/hazard_ptr.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <thread>
#include <vector>

struct counted_node {
    int* destroyed;
    ~counted_node() { ++*destroyed; }
};

int main() {
    constexpr std::size_t owner_capacity =
        flux_foundry::MAX_SLOT / flux_foundry::HP_PER_THREAD;

    std::atomic<std::size_t> ready{0};
    std::atomic<std::size_t> unexpected{0};
    std::atomic<bool> release{false};
    std::vector<std::thread> threads;
    threads.reserve(owner_capacity);

    for (std::size_t i = 0; i < owner_capacity; ++i) {
        threads.emplace_back([&] {
            std::array<flux_foundry::hazard_ptr,
                       flux_foundry::HP_PER_THREAD> holders;
            for (auto& hp : holders) {
                if (!hp.acquire_slot()) {
                    unexpected.fetch_add(1, std::memory_order_relaxed);
                }
            }
            ready.fetch_add(1, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });
    }

    while (ready.load(std::memory_order_acquire) != owner_capacity) {
        std::this_thread::yield();
    }

    {
        flux_foundry::hazard_ptr retrying;
        if (retrying.acquire_slot() != nullptr) {
            unexpected.fetch_add(1, std::memory_order_relaxed);
        }

#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        std::atomic<int*> target{nullptr};
        try {
            retrying.protect(target);
            unexpected.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::runtime_error&) {
        } catch (...) {
            unexpected.fetch_add(1, std::memory_order_relaxed);
        }
#endif

        std::atomic<bool> challenger_observed_full{false};
        std::atomic<bool> challenger_acquired{false};
        std::thread challenger([&] {
            flux_foundry::hazard_ptr hp;
            if (hp.acquire_slot() != nullptr) {
                unexpected.fetch_add(1, std::memory_order_relaxed);
                challenger_observed_full.store(true, std::memory_order_release);
                challenger_acquired.store(true, std::memory_order_release);
                return;
            }

            challenger_observed_full.store(true, std::memory_order_release);
            while (!hp.acquire_slot()) {
                std::this_thread::yield();
            }
            challenger_acquired.store(true, std::memory_order_release);
        });

        while (!challenger_observed_full.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        release.store(true, std::memory_order_release);

        for (auto& thread : threads) {
            thread.join();
        }
        challenger.join();

        if (!challenger_acquired.load(std::memory_order_acquire)) {
            unexpected.fetch_add(1, std::memory_order_relaxed);
        }

        if (retrying.acquire_slot() == nullptr) {
            unexpected.fetch_add(1, std::memory_order_relaxed);
        }
    }

    int parent_destroyed = 0;
    int child_destroyed = 0;
    int dummy_destroyed = 0;
    auto* parent = new counted_node{&parent_destroyed};
    auto* child = new counted_node{&child_destroyed};
    std::atomic<counted_node*> parent_source{parent};
    std::atomic<counted_node*> child_source{child};
    flux_foundry::hazard_ptr parent_hp;
    flux_foundry::hazard_ptr child_hp;

    if (!parent_hp.acquire_slot() || !child_hp.acquire_slot()) {
        return 1;
    }
    parent_hp.protect(parent_source);
    child_hp.protect(child_source);
    parent_source.store(nullptr, std::memory_order_release);
    child_source.store(nullptr, std::memory_order_release);

    flux_foundry::hazard_ptr::retire(parent, [child](counted_node* p) noexcept {
        flux_foundry::hazard_ptr::retire(child);
        delete p;
    });
    parent_hp.unprotect();

    for (int i = 0; i < 31; ++i) {
        flux_foundry::hazard_ptr::retire(new counted_node{&dummy_destroyed});
    }
    child_hp.unprotect();
    for (int i = 0; i < 32; ++i) {
        flux_foundry::hazard_ptr::retire(new counted_node{&dummy_destroyed});
    }

    int null_deleter_calls = 0;
    flux_foundry::hazard_ptr::retire(
        static_cast<counted_node*>(nullptr),
        [&null_deleter_calls](counted_node*) noexcept { ++null_deleter_calls; });

    if (parent_destroyed != 1 || child_destroyed != 1 ||
        dummy_destroyed != 63 || null_deleter_calls != 0) {
        unexpected.fetch_add(1, std::memory_order_relaxed);
    }

    return unexpected.load(std::memory_order_relaxed) == 0
        ? 0
        : 1;
}
