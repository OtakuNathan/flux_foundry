#ifndef FLUX_FOUNDRY_HAZARD_PTR_H
#define FLUX_FOUNDRY_HAZARD_PTR_H

#include <atomic>
#include <thread>
#include <vector>
#include <stdexcept>
#include <cstdlib>

#include "../base/traits.h"
#include "../memory/flat_storage.h"
#include "../utility/callable_wrapper.h"
#include "../utility/back_off.h"

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
    
    struct alignas(OPTIMIZED_ALIGN) hazard_record {
        std::atomic<std::thread::id> tid{std::thread::id()};
        std::atomic<const void*> ptr{nullptr};
        bool used = false;
    };

private:
    struct retire_record {
        compressed_pair<void*, deleter_t> p;

        retire_record(const retire_record&) = delete;
        retire_record& operator=(const retire_record&) = delete;
        retire_record(retire_record&&) noexcept = default;
        retire_record& operator=(retire_record&&) noexcept  = default;

        retire_record() noexcept
            : p(nullptr, [](void*) noexcept {}) {
        }

        template <typename Deleter>
        retire_record(void* p_, Deleter _deleter) noexcept
            : p(p_, std::move(_deleter)) {}
    };

    template <typename T, typename Deleter>
    struct erased_deleter {
        Deleter deleter;

        explicit erased_deleter(Deleter d) noexcept
            : deleter(std::move(d)) {}

        void operator()(void* p) noexcept {
            deleter(static_cast<T*>(p));
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
        bool           sweeping;

        hp_owner()
#if !FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
            noexcept
#endif
         : my_slots{}, list{new retire_list}, retire_count{}, sweeping{false} {
            // Construct the process-wide manager before this TLS owner finishes
            // initialization.  The resulting destruction order keeps the manager
            // alive while the owner releases cached slots at thread exit.
            (void)instance();
        }

        hp_owner(const hp_owner&) = delete;
        hp_owner& operator=(const hp_owner&) = delete;
        hp_owner(hp_owner&&) noexcept = delete;
        hp_owner& operator=(hp_owner&&) noexcept  = delete;

        ~hp_owner() noexcept {
            for (size_t i = 0; i < HP_PER_THREAD; ++i) {
                if (my_slots[i]) {
                    my_slots[i]->ptr.store(nullptr, std::memory_order_release);
                    my_slots[i]->used = false;
                    // Publish the record as free only after all thread-local state
                    // has been reset.  A successful acquire CAS on tid then owns
                    // every subsequent access to the non-atomic used flag.
                    my_slots[i]->tid.store(std::thread::id(), std::memory_order_release);
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
            const size_t batch_count = records.size();
            size_t survivor_count = 0;
            sweeping = true;

            for (size_t i = 0; i < batch_count; ++i) {
                if (hp_mgr::is_hazard(records[i].p.first())) {
                    if (survivor_count != i) {
                        records[survivor_count] = std::move(records[i]);
                    }
                    ++survivor_count;
                } else {
                    // Move the record out before invoking user code.  Its deleter
                    // may retire more objects and reallocate the backing vector.
                    retire_record reclaiming(std::move(records[i]));
                    reclaiming.p.second()(reclaiming.p.first());
                }
            }

            // Retirements made by reclaim callbacks belong to the next batch.
            const size_t appended_count = records.size() - batch_count;
            for (size_t i = 0; i < appended_count; ++i) {
                records[survivor_count + i] =
                    std::move(records[batch_count + i]);
            }
            records.resize(survivor_count + appended_count);
            sweeping = false;
        }

        hazard_record* acquire_slot() noexcept {
            for (size_t i = 0; i < HP_PER_THREAD; ++i) {
                if (my_slots[i] && !my_slots[i]->used) {
                    my_slots[i]->used = true;
                    return my_slots[i];
                }
            }

            auto& mgr = instance();
            const std::thread::id tid = std::this_thread::get_id();
            const std::thread::id empty_id{};

            for (size_t local = 0; local < HP_PER_THREAD; ++local) {
                if (my_slots[local]) continue;

                for (size_t global = 0; global < MAX_SLOT; ++global) {
                    std::thread::id expected = empty_id;
                    if (mgr.slots[global].tid.compare_exchange_strong(
                            expected, tid,
                            std::memory_order_acq_rel,
                            std::memory_order_relaxed)) {
                        auto* record = &mgr.slots[global];
                        record->ptr.store(nullptr, std::memory_order_release);
                        record->used = true;
                        my_slots[local] = record;
                        return record;
                    }
                }
                return nullptr;
            }
            return nullptr;
        }

        static hp_owner& get_tls_owner() {
            thread_local hp_owner owner;
            return owner;
        }
    };

    static hazard_record* acquire_slot()
#if !FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        noexcept
#endif
    {
        return hp_owner::get_tls_owner().acquire_slot();
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
        using deleter_type = std::decay_t<Deleter>;
        using erased_deleter_type = erased_deleter<T, deleter_type>;
        static_assert(std::is_nothrow_move_constructible<deleter_type>::value,
                      "Deleter must be nothrow move constructible");
        static_assert(noexcept(std::declval<deleter_type&>()(std::declval<T*>())),
                      "Deleter invocation must be noexcept");
        static_assert(sizeof(erased_deleter_type) <= callable_wrapper_sbo_size,
                      "Deleter must fit the callable_wrapper SBO; allocate state externally and capture a handle");
        static_assert(alignof(erased_deleter_type) <= alignof(std::max_align_t),
                      "Deleter alignment exceeds the callable_wrapper SBO alignment");

        if (!p) return;

        auto& owner = hp_owner::get_tls_owner();

        if (!(++owner.retire_count % (RETIRE_BATCH >> 1)) && !owner.sweeping) {
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
            vec.emplace_back(p, erased_deleter_type(std::move(deleter)));
        }
    }

    static bool is_hazard(const void* ptr) noexcept {
        auto& self = instance();
        for (size_t i = 0; i < MAX_SLOT; ++i) {
            // seq_cst: must observe the same global total order as protect()'s
            // slot->store, otherwise the reclaimer could miss a just-published
            // hazard and delete the pointer.
            if (self.slots[i].ptr.load(std::memory_order_seq_cst) == ptr) {
                return true;
            }
        }
        return false;
    }
};

} // namespace detail

// hazard_ptr supports deferred slot acquisition and
// explicit recovery in no-exception builds via available() + acquire_slot().
struct hazard_ptr {
private:
    using hazard_record = typename detail::hp_mgr::hazard_record;
    hazard_record* slot;

public:
    hazard_ptr() noexcept : slot(nullptr) {}

    template <typename T>
    explicit hazard_ptr(std::atomic<T*>& target)
#if !FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
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

    hazard_record* acquire_slot()
#if !FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        noexcept
#endif
    {
        return slot ? slot : (slot = detail::hp_mgr::acquire_slot());
    }

    template <typename T>
    T* get() const noexcept {
        return slot ? static_cast<T*>(const_cast<void*>(slot->ptr.load(std::memory_order_acquire))) : nullptr;
    }

    template <typename T>
    T* protect(std::atomic<T*>& target)
#if !FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
        noexcept
#endif
    {
        if (!slot) {
            slot = detail::hp_mgr::acquire_slot();
            if (!slot) {
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
                throw std::runtime_error("Hazard Pointer Slots Exhausted!");
#else
                std::abort();
#endif
            }
        }

        // seq_cst on slot->store and re-check load is required to establish
        // a global total order with the reclaimer's is_hazard scan.
        // acquire/release alone cannot guarantee ordering across two
        // different atomic variables (slot->ptr and target).
        T* p = nullptr;
        do {
            p = target.load(std::memory_order_relaxed);
            slot->ptr.store(p, std::memory_order_seq_cst);
        } while (p != target.load(std::memory_order_seq_cst));
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
        detail::hp_mgr::retire(p, std::move(d));
    }
    
    static bool sweep_and_reclaim() noexcept {
        return detail::hp_mgr::instance().sweep_and_reclaim_impl();
    }
};

} // namespace flux_foundry

#endif
