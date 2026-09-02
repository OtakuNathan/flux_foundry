#include "../memory/hazard_ptr.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

struct node {
    node* next{nullptr};
    std::atomic<std::size_t>* destroyed;

    explicit node(std::atomic<std::size_t>& counter) noexcept
        : destroyed(&counter) {}

    ~node() {
        destroyed->fetch_add(1, std::memory_order_relaxed);
    }
};

class treiber_stack {
public:
    void push(node* value) noexcept {
        value->next = head_.load(std::memory_order_relaxed);
        while (!head_.compare_exchange_weak(
            value->next, value,
            std::memory_order_release,
            std::memory_order_relaxed)) {}
    }

    bool pop(flux_foundry::hazard_ptr& hp) {
        for (;;) {
            node* current = hp.protect(head_);
            if (!current) {
                hp.unprotect();
                return false;
            }

            node* next = current->next;
            if (head_.compare_exchange_weak(
                    current, next,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                hp.unprotect();
                flux_foundry::hazard_ptr::retire(current);
                return true;
            }
        }
    }

    node* release_all() noexcept {
        return head_.exchange(nullptr, std::memory_order_acq_rel);
    }

private:
    std::atomic<node*> head_{nullptr};
};

std::size_t parse_arg(char* arg, std::size_t fallback) {
    if (!arg) return fallback;
    const auto value = std::strtoull(arg, nullptr, 10);
    return value ? static_cast<std::size_t>(value) : fallback;
}

} // namespace

int main(int argc, char** argv) {
    const std::size_t producer_count =
        parse_arg(argc > 1 ? argv[1] : nullptr, 2);
    const std::size_t consumer_count =
        parse_arg(argc > 2 ? argv[2] : nullptr, 2);
    const std::size_t items_per_producer =
        parse_arg(argc > 3 ? argv[3] : nullptr, 10000);
    const std::size_t total = producer_count * items_per_producer;

    treiber_stack stack;
    std::atomic<std::size_t> destroyed{0};
    std::atomic<std::size_t> popped{0};
    std::vector<std::thread> threads;
    threads.reserve(producer_count + consumer_count);

    for (std::size_t i = 0; i < producer_count; ++i) {
        threads.emplace_back([&] {
            for (std::size_t item = 0; item < items_per_producer; ++item) {
                stack.push(new node(destroyed));
            }
        });
    }

    for (std::size_t i = 0; i < consumer_count; ++i) {
        threads.emplace_back([&] {
            flux_foundry::hazard_ptr hp;
            while (!hp.acquire_slot()) {
                std::this_thread::yield();
            }

            while (popped.load(std::memory_order_acquire) < total) {
                if (stack.pop(hp)) {
                    popped.fetch_add(1, std::memory_order_release);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    for (node* current = stack.release_all(); current;) {
        node* next = current->next;
        delete current;
        current = next;
    }

    while (flux_foundry::hazard_ptr::sweep_and_reclaim()) {}

    return popped.load(std::memory_order_relaxed) == total &&
           destroyed.load(std::memory_order_relaxed) == total
        ? 0
        : 1;
}
