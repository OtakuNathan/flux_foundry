#ifndef LITE_FNDS_FLOW_AGGREGATOR_H
#define LITE_FNDS_FLOW_AGGREGATOR_H

#include <cstdlib>
#include <atomic>

#include "../base/inplace_base.h"
#include "../memory/lite_ptr.h"
#include "../memory/aligned_alloc.h"
#include "flow_blueprint.h"

/**
 * flow_aggregator: Lock-free result aggregator for fork/join patterns
 *
 * Provides primitives for checking result readiness. Users decide how to wait:
 *
 * Example 1: Spin wait (lowest latency)
 *   while (!agg.is_all_ready()) { }
 *
 * Example 2: Yield wait (CPU-friendly)
 *   while (!agg.is_all_ready()) { std::this_thread::yield(); }
 *
 * Example 3: Condition variable (event-driven)
 *   std::unique_lock lock(mtx);
 *   cv.wait(lock, [&]{ return agg.is_all_ready(); });
 *   (Requires wrapping delegate to notify CV)
 *
 * Example 4: Async polling (event loop)
 *   if (agg.is_all_ready()) { process(agg.value()); }
 *
 * Example 5: Wait for any (first-wins)
 *   while (!agg.is_any_ready()) { yield(); }
 *   if (agg.is_slot_ready<0>()) { handle(get<0>(agg.value())); }
 *
 * Flow provides mechanism, you provide policy.
 */

namespace lite_fnds {
    template <typename... Ts>
    struct flow_aggregator {
        static_assert(conjunction_v<is_result_t<Ts>...>,
            "All the types in the template param pack must be result_t<T, E>");
        constexpr static size_t N = sizeof...(Ts);
        using storage_t = flat_storage<std::decay_t<Ts>...>;
    private:
        enum class slot_state : uint8_t {
            empty,
            occupied,
            full,
        };

        struct alignas(CACHE_LINE_SIZE) Data {
            std::atomic<size_t> ready_count;

            // Tips: if this is checked very frequently, each flag should be aligned by 64
            std::atomic<slot_state> slot_ready[N];
            storage_t val;

            Data() 
                : ready_count { 0 }
                , val { Ts(error_tag, typename Ts::error_type {})... } {
                for (size_t i = 0; i < N; ++i) {
                    slot_ready[i].store(slot_state::empty, std::memory_order_relaxed);
                }
            }

            static auto make_shared() {
                return make_lite_ptr<Data>();
            }
        };

    public:
        template <size_t I>
        struct delegate {
        private:
            using elem_type = flat_storage_element_t<I, storage_t>;
            lite_ptr<Data> data;
             
        public:
            delegate() = delete;

            explicit delegate(lite_ptr<Data> d) noexcept
                : data(std::move(d)) {
            }

            // Only call emplace once per delegate
            template <typename... Us, 
#if LFNDS_HAS_EXCEPTIONS
                std::enable_if_t<std::is_constructible<elem_type, Us&&...>::value
#else
                std::enable_if_t<std::is_nothrow_constructible<elem_type, Us&&...>::value
#endif
            >* = nullptr>
            bool emplace(Us&&... args) noexcept {
                // if this slot is already used. return false;
                slot_state expected = slot_state::empty;
                if (!data->slot_ready[I].compare_exchange_strong(expected, slot_state::occupied,
                                                                std::memory_order_release, std::memory_order_relaxed)) {
                    return false;
                }

                elem_type& e = get<I>(data->val);
#if LFNDS_COMPILER_HAS_EXCEPTIONS
                try {
#endif
                    e = elem_type(std::forward<Us>(args)...);
#if LFNDS_COMPILER_HAS_EXCEPTIONS
                } catch (...) {
                    e.emplace_error(std::current_exception());
                }
#endif

                data->slot_ready[I].store(slot_state::full, std::memory_order_release);
                data->ready_count.fetch_add(1, std::memory_order_release);
                return true;
            }
        };
        
        flow_aggregator() : 
            data(Data::make_shared()) {
#if !LFNDS_COMPILER_HAS_EXCEPTIONS
            assert(data && "failed to allocate aggregator data.");
#endif
        }

        flow_aggregator(const flow_aggregator&) = default;
        flow_aggregator& operator=(const flow_aggregator&) = default;
        flow_aggregator(flow_aggregator&&) = default;
        flow_aggregator& operator=(flow_aggregator&&) = default;
        ~flow_aggregator() noexcept = default;

        bool is_any_ready() const noexcept {
            return this->data->ready_count.load(std::memory_order_acquire) != 0;
        }

        bool is_all_ready() const noexcept {
            return this->data->ready_count.load(std::memory_order_acquire) == N;
        }
        
        template <size_t I>
        bool is_slot_ready() const noexcept {
            static_assert(I < N, "flow_aggregator index out of range");
            return this->data->slot_ready[I].load(std::memory_order_acquire) == slot_state::full;
        }
        
        // You should only create a delegate for each slot once
        template <size_t I>
        auto delegate_for() noexcept {
            static_assert(I < N, "flow_aggregator index out of range");
            return delegate<I>(data);
        }
        
        size_t value_got() const noexcept {
            return this->data->ready_count.load(std::memory_order_acquire);
        }
        
        storage_t& value() & noexcept {
            return this->data->val;
        }

        const storage_t& value() const& noexcept {
            return this->data->val;
        }
        
        storage_t&& value() && noexcept {
            return std::move(this->data->val);
        }
    private:
        lite_ptr<Data> data;
    };

    template <typename ... BPs>
    auto make_aggregator(const BPs&...) {
        static_assert(conjunction_v<flow_impl::is_blueprint<BPs>...>,
            "you can only use this function with blueprints as input.");
        return flow_aggregator<typename BPs::O_t...>();
    }

    template <typename... BPs>
    auto make_aggregator_t() {
        static_assert(conjunction_v<flow_impl::is_blueprint<BPs>...>, "BPs should be blueprints");
        return flow_aggregator<typename BPs::O_t...>();
    }
    }

#endif