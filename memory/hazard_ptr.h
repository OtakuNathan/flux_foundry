#ifndef FLUX_FOUNDRY_HAZARD_PTR_H
#define FLUX_FOUNDRY_HAZARD_PTR_H

#include <atomic>
#include <thread>
#include <memory>
#include <vector>
#include <stdexcept>
#include <cstdlib>
#include <cassert>

#include "../base/traits.h"
#include "../memory/flat_storage.h"
#include "../utility/callable_wrapper.h"
#include "../utility/back_off.h"
#include "../utility/static_list.h"

namespace flux_foundry {

struct hazard_ptr;

template <typename Callable>
using callable_t = callable_wrapper<Callable>;

constexpr static size_t MAX_SLOT = 128;
constexpr static size_t HP_PER_THREAD = 2;
constexpr static size_t RETIRE_BATCH = 64;

namespace detail {
struct hp_mgr {
    using deleter_t = callable_t<void(void*)>;
    
    struct alignas(CACHE_LINE_SIZE) hazard_record {
        std::atomic<std::thread::id> tid{std::thread::id()};
        std::atomic<const void*> ptr{nullptr};
        bool used = false;
    };

private:
    struct retire_record {
        static void stub_free(void*) noexcept { }

        compressed_pair<void*, deleter_t> p;

        retire_record(const retire_record&) = delete;
        retire_record& operator=(const retire_record&) = delete;
        retire_record(retire_record&&) noexcept = default;
        retire_record& operator=(retire_record&&) noexcept  = default;

        retire_record() noexcept
            : p(nullptr, stub_free) {
        }

        template <typename Deleter>
        retire_record(void* p_, Deleter _deleter) noexcept
            : p(p_, std::move(_deleter)) {
            static_assert(noexcept(std::declval<Deleter>()(nullptr)), "Deleter must be noexcept");
        }
    };

    struct retire_list {
        retire_list* next{nullptr};
        std::vector<retire_record> retired;
        retire_list() { retired.reserve(RETIRE_BATCH); }
    };

public:
    static hp_mgr& instance() noexcept {
        static hp_mgr instance;
        return instance;
    }

    ~hp_mgr() noexcept { 
        sweep_and_reclaim_impl(); 
    }

    hazard_record slots[MAX_SLOT];
    std::atomic<retire_list*> orphans{nullptr};

    bool sweep_and_reclaim_impl() noexcept {
        using std::swap;
        retire_list* orphans_ = this->orphans.exchange(nullptr, std::memory_order_acq_rel);
        retire_list **it = &orphans_, *p = *it;
        
        for (; p;) {
            auto& records = p->retired;
            auto count = records.size();
            for (size_t i = 0; i < count;) {
                auto& record = records[i];
                if (is_hazard(record.p.first())) {
                    ++i;
                } else {
                    record.p.second()(record.p.first());
                    swap(record, records[count - 1]);
                    --count;
                }
            }
            if (count > 0) {
                records.resize(count);
                it = &(*it)->next;
            } else {
                *it = p->next;
                delete p;
            }
            p = *it;
        }

        if (orphans_) {
            *it = this->orphans.load(std::memory_order_acquire);
            for (backoff_strategy<> backoff;
                !this->orphans.compare_exchange_weak(*it, orphans_, std::memory_order_acq_rel, std::memory_order_acquire);
                backoff.yield()) {}
        }
        return orphans_;
    }

    struct hp_owner {
        hazard_record *my_slots[HP_PER_THREAD];
        retire_list    *list;
        size_t         retire_count;

        hp_owner()
#if !defined(__cpp_exceptions)
            noexcept
#endif
         : my_slots{}, list{new retire_list}, retire_count{} {
            auto& mgr = instance();
            size_t acquired = 0;
            std::thread::id tid = std::this_thread::get_id();
            std::thread::id empty_id = std::thread::id();

            for (size_t i = 0; i < MAX_SLOT && acquired < HP_PER_THREAD; ++i) {
                if (mgr.slots[i].tid.load(std::memory_order_relaxed) != empty_id) continue;
                if (mgr.slots[i].tid.compare_exchange_strong(empty_id, tid,
                                            std::memory_order_acq_rel, std::memory_order_relaxed)) {
                    my_slots[acquired++] = &mgr.slots[i];
                    mgr.slots[i].ptr.store(nullptr, std::memory_order_release);
                }
                empty_id = std::thread::id();
            }

            if (acquired < HP_PER_THREAD) {
                while (acquired--) my_slots[acquired]->tid.store(std::thread::id(), std::memory_order_release);
#if defined(__cpp_exceptions)
                throw std::runtime_error("Hazard Pointer Slots Exhausted!");
#else
                std::abort();
#endif
            }
        }

        hp_owner(const hp_owner&) = delete;
        hp_owner& operator=(const hp_owner&) = delete;
        hp_owner(hp_owner&&) noexcept = delete;
        hp_owner& operator=(hp_owner&&) noexcept  = delete;

        ~hp_owner() noexcept {
            for (size_t i = 0; i < HP_PER_THREAD; ++i) {
                if (my_slots[i]) {
                    my_slots[i]->ptr.store(nullptr, std::memory_order_release);
                    my_slots[i]->tid.store(std::thread::id(), std::memory_order_release);
                    my_slots[i]->used = false;
                }
            }
            sweep_and_reclaim();
            if (list->retired.empty()) {
                delete list;
            } else {
                auto &mgr = instance();
                list->next = mgr.orphans.load(std::memory_order_acquire);
                for (backoff_strategy<> backoff;
                    !mgr.orphans.compare_exchange_weak(list->next, list,
                        std::memory_order_acq_rel, std::memory_order_acquire);
                    backoff.yield());
            }
        }

        void sweep_and_reclaim() noexcept {
            auto& records = list->retired;
            auto count = records.size();
            for (size_t i = 0; i < count; ) {
                auto& record = records[i];
                if (hp_mgr::is_hazard(record.p.first())) {
                    ++i;
                } else {
                    using std::swap;
                    record.p.second()(record.p.first());
                    swap(record, records[count - 1]);
                     --count;
                }
            }
            if (count != records.size()) records.resize(count);
        }

        static hp_owner& get_tls_owner() {
            thread_local hp_owner owner;
            return owner;
        }
    };

    static hazard_record* acquire_slot() noexcept {
        auto& owner = hp_owner::get_tls_owner();
        for (size_t i = 0; i < HP_PER_THREAD; ++i) {
            if (!owner.my_slots[i]->used) {
                owner.my_slots[i]->used = true;
                return owner.my_slots[i];
            }
        }
        return nullptr;
    }

    static void free_local_slot(hazard_record* record) noexcept {
        if (record) {
            record->ptr.store(nullptr, std::memory_order_release);
            record->used = false;
        }
    }

    // Static implementations called by hazard_ptr
    template <typename T, typename Deleter>
    static void retire(T* p, Deleter deleter) {
        auto& owner = hp_owner::get_tls_owner();

        if (!(++owner.retire_count % (RETIRE_BATCH >> 1))) {
            owner.sweep_and_reclaim();
            if (owner.list->retired.empty()) {
                hp_mgr::instance().sweep_and_reclaim_impl();
            }
        }

        if (!is_hazard(p)) {
            deleter(p);
        } else {
            auto& vec = owner.list->retired;
            if (vec.size() == vec.capacity()) {
                size_t new_cap = vec.capacity() == 0 ? RETIRE_BATCH : vec.capacity() * 2;
                vec.reserve(new_cap);
            }
            vec.emplace_back(p, [deleter = std::move(deleter)](void* _p) noexcept {
                deleter(static_cast<T*>(_p));
            });
        }
    }

    static bool is_hazard(const void* ptr) noexcept {
        auto& self = instance();
        for (size_t i = 0; i < MAX_SLOT; ++i) {
            if (self.slots[i].ptr.load(std::memory_order_acquire) == ptr) {
                return true;
            }
        }
        return false;
    }
};

} // namespace detail

struct hazard_ptr {
private:
    using hazard_record = typename detail::hp_mgr::hazard_record;
    hazard_record* slot;

public:
    hazard_ptr() noexcept : slot(nullptr) {}

    template <typename T>
    explicit hazard_ptr(std::atomic<T*>& target)
#if !defined(__cpp_exceptions)
        noexcept
#endif
        : slot(detail::hp_mgr::acquire_slot()) {
        protect(target);
    }

    ~hazard_ptr() noexcept {
        if (slot) detail::hp_mgr::free_local_slot(slot);
    }

    hazard_ptr(const hazard_ptr&) = delete;
    hazard_ptr& operator=(const hazard_ptr&) = delete;
    hazard_ptr(hazard_ptr&& hp) noexcept = delete;
    hazard_ptr& operator=(hazard_ptr&& hp) noexcept = delete;

    bool available() const noexcept { return slot != nullptr; }

    hazard_record* acquire_slot() noexcept {
        return slot ? slot : (slot = detail::hp_mgr::acquire_slot());
    }

    template <typename T>
    T* get() const noexcept {
        return slot ? static_cast<T*>(const_cast<void*>(slot->ptr.load(std::memory_order_acquire))) : nullptr;
    }

    template <typename T>
    T* protect(std::atomic<T*>& target)
#if !defined(__cpp_exceptions)
        noexcept
#endif
    {
        if (!slot) {
            slot = detail::hp_mgr::acquire_slot();
            if (!slot) {
#if defined(__cpp_exceptions)
                throw std::runtime_error("Hazard Pointer Slots Exhausted!");
#else
                std::abort();
#endif
            }
        }

        T* p = nullptr;
        do {
            p = target.load(std::memory_order_acquire);
            slot->ptr.store(p, std::memory_order_release);
        } while (p != target.load(std::memory_order_acquire));
        return p;
    }

    void unprotect() noexcept {
        if (slot) slot->ptr.store(nullptr, std::memory_order_release);
    }

    template <typename T>
    static void retire(T* p) {
        detail::hp_mgr::retire(p, [](T* _p) noexcept { 
            delete _p; 
        });
    }

    template <typename T, typename Deleter>
    static void retire(T* p, Deleter d) {
        static_assert(noexcept(std::declval<Deleter>()(std::declval<T*>())), "Deleter must be noexcept");
        detail::hp_mgr::retire(p, std::move(d));
    }
    
    static bool sweep_and_reclaim() noexcept {
        return detail::hp_mgr::instance().sweep_and_reclaim_impl();
    }
};

} // namespace flux_foundry

#endif