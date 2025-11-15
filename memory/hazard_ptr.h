#ifndef __LITE_FNDS_HAZARD_PTR_H__
#define __LITE_FNDS_HAZARD_PTR_H__

#include <atomic>
#include <thread>
#include <functional>

#include "../utility/static_list.h"

namespace lite_fnds {
struct hazard_ptr;

template <typename Callable>
using callable_t = std::function<Callable>;

struct hp_mgr {
public:
    static constexpr size_t max_slot = 128;
    using deleter_t = callable_t<void(void*)>;

#ifdef USE_HEAP_ALLOCATED
    struct retire_list_node {
        void* ptr;
        deleter_t deleter;

        retire_list_node* next;

        retire_list_node(const retire_list_node&) = delete;
        retire_list_node& operator=(const retire_list_node&) = delete;
        
        template <typename Deleter>
        retire_list_node(void* p, Deleter _deleter)
            : ptr(p), deleter(std::move(_deleter)) {
        }
    };

    static std::atomic<retire_list_node*> retire_list;

    static void append_to_retire_list(retire_list_node* node) {
        auto old_head = retire_list.load(std::memory_order_relaxed);
        do {
            node->next = old_head;
        } while (!retire_list.compare_exchange_weak(old_head, node,
            std::memory_order_release, std::memory_order_acquire));
    }

    static void sweep_and_reclaim() noexcept {
        auto list = retire_list.exchange(nullptr, std::memory_order_acq_rel);
        for (auto p = list; p;) {
            auto nxt = p->next;
            if (!is_hazard(p->ptr)) {
                p->deleter(p->ptr);
                delete p;
            } else {
                p->next = nullptr;
                append_to_retire_list(p);
            }
            p = nxt;
        }
    }

    template <typename T>
    static void retire(T* p) {
        if (!is_hazard(p)) {
            delete p;
        } else {
            // append a new node to reclaim list
            std::unique_ptr<retire_list_node> node = std::make_unique<retire_list_node>(p, [](void* p) {
                      delete static_cast<T*>(p);
                  });
            append_to_retire_list(node.get());
            node.release();
        }
    }

    template <typename T, typename Deleter>
    static void retire(T* p, Deleter deleter) {
        static_assert(noexcept(std::declval<Deleter>()(std::declval<T*>())), "Deleter(T*) must be noexcept");

        if (!is_hazard(p)) {
            deleter(p);
        } else {
            // append a new node to reclaim list
            std::unique_ptr<retire_list_node> node
                = std::make_unique<retire_list_node>(p, [deleter = std::move(deleter)](void* p) {
                      deleter(static_cast<T*>(p));
                  });

            append_to_retire_list(node.get());
            node.release();
        }
    }
#else
    struct retire_list_node {
        void* ptr;
        deleter_t deleter;

        retire_list_node(const retire_list_node&) = delete;
        retire_list_node& operator=(const retire_list_node&) = delete;

        retire_list_node(retire_list_node&&) = default;
        retire_list_node& operator=(retire_list_node&&) = default;

        template <typename Deleter>
        retire_list_node(void* p, Deleter _deleter)
            : ptr(p)
            , deleter(std::move(_deleter)) {
        }
    };

    static static_list<retire_list_node, max_slot> retire_list;

    static void sweep_and_reclaim() noexcept {
        for (inplace_t<retire_list_node> node = retire_list.pop(); 
            node.has_value(); 
            node = retire_list.pop()) {
         
            if (!is_hazard(node.get().ptr)) {
                node.get().deleter(node.get().ptr);
            } else {
                retire_list.emplace(node.steal());
            }
        }
    }

    template <typename T>
    static void retire(T* p) {
        if (!is_hazard(p)) {
            delete p;
        } else {
            // append a new node to reclaim list
            retire_list.emplace(p, [](void* p) noexcept {
                delete static_cast<T*>(p);
            });
        }
    }

    template <typename T, typename Deleter>
    static void retire(T* p, Deleter deleter) {
        static_assert(noexcept(std::declval<Deleter>()(std::declval<T*>())), 
            "Deleter(T*) must be noexcept");
        if (!is_hazard(p)) {
            deleter(p);
        } else {
            // append a new node to reclaim list
            retire_list.emplace(p, [deleter = std::move(deleter)](void* p) noexcept {
                deleter(static_cast<T*>(p));
            });
        }
    }

#endif
    struct alignas(CACHE_LINE_SIZE) hazard_record {
        std::atomic<std::thread::id> tid;
        std::atomic<const void*> ptr;
        pad_t<sizeof(tid) + sizeof(ptr)> pad;
    };
    static hazard_record record[max_slot];
    friend struct hazard_ptr;

    static hazard_record* acquire_slot(std::thread::id tid) noexcept {
        for (int i = 0; i < max_slot; ++i) {
            auto exp = std::thread::id();
            if (record[i].tid.compare_exchange_strong(exp, tid,
                    std::memory_order_release, std::memory_order_relaxed)) {
                return &record[i];
            }
        }
        return nullptr;
    }

    static bool is_hazard(const void* ptr) noexcept {
        for (size_t i = 0; i < max_slot; ++i) {
            if (record[i].ptr.load(std::memory_order_acquire) == ptr) {
                return true;
            }
        }
        return false;
    }
};

struct hazard_ptr {
    using hazard_record = typename hp_mgr::hazard_record;
    hazard_record* slot;

public:
    hazard_ptr() noexcept
        : slot { hp_mgr::acquire_slot(std::this_thread::get_id()) } {
    }

    ~hazard_ptr() noexcept {
        release_slot();
    }

    hazard_ptr(const hazard_ptr&) = delete;
    hazard_ptr& operator=(const hazard_ptr&) = delete;

    hazard_ptr(hazard_ptr&& hp) noexcept
        : slot(hp.slot) {
        hp.slot = nullptr;
    }

    hazard_ptr& operator=(hazard_ptr&& hp) noexcept {
        if (this != &hp) {
            hazard_ptr tmp(std::move(hp));
            this->swap(tmp);
        }
        return *this;
    }

    bool available() noexcept {
        return slot != nullptr;
    }

    hazard_record* acquire_slot() noexcept {
        return slot ? slot : slot = hp_mgr::acquire_slot(std::this_thread::get_id());
    }

    void swap(hazard_ptr& rhs) noexcept {
        using std::swap;
        swap(slot, rhs.slot);
    }

    void protect(const void* p) noexcept {
        slot->ptr.store(p, std::memory_order_release);
    }

    void unprotect() noexcept {
        slot->ptr.store(nullptr, std::memory_order_release);
    }

    void release_slot() noexcept {
        if (slot) {
            unprotect();
            slot->tid.store(std::thread::id(), std::memory_order_release);
            slot = nullptr;
        }
    }

    static bool is_hazard(const void* p) noexcept {
        return hp_mgr::is_hazard(p);
    }

    template <typename T>
    T* acquire_protected(std::atomic<T*>& target) noexcept {
        T* p {};
        do {
            p = target.load(std::memory_order_acquire);
            protect(p);
        } while (p != target.load(std::memory_order_acquire));
        return p;
    }
};

void swap(hazard_ptr& lhs, hazard_ptr& rhs) noexcept {
    lhs.swap(rhs);
}

}

#endif
