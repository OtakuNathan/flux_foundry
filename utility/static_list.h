#ifndef LITE_FNDS_STATIC_LIST_H
#define LITE_FNDS_STATIC_LIST_H

#include <atomic>
#include <type_traits>
#include <utility>

#include "../memory/inplace_t.h"
#include "../base/traits.h"
#include "yield.h"

namespace lite_fnds {
    template <typename T, size_t capacity>
    struct static_list {
        using storage_t = std::decay_t<T>;

        static_assert(std::is_nothrow_move_constructible<T>::value,
                      "T must be no throw move constructible.");
        static_assert(capacity < 0xffffffffL, "capacity must be less than 4 GB.");
        static_assert((capacity & (capacity - 1)) == 0, "capacity must be 2^n.");

    private:
        struct node {
            raw_inplace_storage_base<storage_t> satellite;
#ifdef TSAN_CLEAR
            std::atomic<uint64_t> next;
#else
            uint64_t next;
#endif
        };

        static constexpr uint64_t cal_offset() noexcept {
            uint64_t i = 0;
            for (auto cap = capacity; cap >= 1;) {
                cap >>= 1, ++i;
            }
            return i;
        }

        constexpr static uint64_t off = cal_offset(),
                offset_msk = (uint64_t{1} << off) - 1,
                seq_msk = ((~offset_msk) >> off), empty_tag = capacity;

        constexpr static uint64_t make_seq(uint64_t seq, uint64_t offset) noexcept {
            return ((seq << off) | offset);
        }

        constexpr static uint64_t get_seq(uint64_t tag) noexcept {
            return (tag >> off) & seq_msk;
        }

        static constexpr uint64_t get_offset(uint64_t tag) noexcept {
            return tag & offset_msk;
        }

        alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> head_;
        pad_t<sizeof(head_)> _pad1;

        alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> free_;
        pad_t<sizeof(free_)> _pad2;

        node nodes[capacity];

        uint64_t pop_from_list(std::atomic<uint64_t>& head) noexcept {
            uint64_t seq = 0, offset = 0;
            uint64_t h_ = head.load(std::memory_order_acquire);
            backoff_strategy<> backoff;
            for (;; backoff.yield()) {
                if (h_ == empty_tag) {
                    return empty_tag;
                }

                seq = get_seq(h_), offset = get_offset(h_);
#if defined(TSAN)
                TSAN_CONSUME(&nodes[offset]);
#endif

#ifdef TSAN_CLEAR
                auto next = nodes[offset].next.load(std::memory_order_relaxed);
#else
                auto next = nodes[offset].next;
#endif

                if (head.compare_exchange_weak(h_, next,
                                               std::memory_order_acq_rel, std::memory_order_acquire)) {
                    break;
                }
            }
            return make_seq(seq, offset);
        }

        uint64_t append_to_list(std::atomic<uint64_t>& head, uint64_t fptr) noexcept {
            uint64_t h_ = head.load(std::memory_order_acquire);;
            backoff_strategy<> backoff;
            for (;; backoff.yield()) {
#ifdef TSAN_CLEAR
                nodes[get_offset(fptr)].next.store(h_, std::memory_order_relaxed);
#else
                nodes[get_offset(fptr)].next = h_;
#endif
#if defined(TSAN)
                TSAN_PUBLISH(&nodes[get_offset(fptr)]);
#endif
                if (head.compare_exchange_weak(h_, fptr,
                                               std::memory_order_acq_rel, std::memory_order_acquire)) {
                    break;
                }
            }
            return h_;
        }

    public:
        static_list() noexcept
                : head_(empty_tag), free_(make_seq(0, 0)) {
            for (size_t i = 0; i < capacity; ++i) {
#ifdef TSAN_CLEAR
                nodes[i].next.store(i + 1, std::memory_order_relaxed);
#else
                nodes[i].next = i + 1;
#endif
            }
        }

        static_list(const static_list&) = delete;
        static_list& operator=(const static_list&) = delete;
        static_list(static_list&&) = delete;
        static_list& operator=(static_list&&) = delete;

        // should only be called when shutting down the program.
        ~static_list() noexcept {
            inplace_t<storage_t> val;
            do {
                val = pop();
            } while (val.has_value());
        }

        template <typename T_ = storage_t,
                std::enable_if_t<std::is_nothrow_copy_constructible<T_>::value>* = nullptr>
        bool push(const storage_t& val) noexcept {
            auto h_ = pop_from_list(free_);
            if (h_ == empty_tag) {
                return false;
            }


            auto seq_ = get_seq(h_), offset = get_offset(h_);
            auto& slot = nodes[offset];
            slot.satellite.construct(val);
            append_to_list(head_, make_seq((seq_ + 1) & seq_msk, offset));

            return true;
        }

#if LFNDS_HAS_EXCEPTIONS
        template <typename T_ = storage_t,
                std::enable_if_t<conjunction_v<negation<std::is_nothrow_copy_constructible<T_>>,
                        std::is_copy_constructible<T_>> >* = nullptr>
        bool push(const storage_t& val) {
            T_ tmp(val);

            auto h_ = pop_from_list(free_);
            if (h_ == empty_tag) {
                return false;
            }

            auto seq_ = get_seq(h_), offset = get_offset(h_);
            auto& slot = nodes[offset];
            slot.satellite.construct(std::move(tmp));
            append_to_list(head_, make_seq((seq_ + 1) & seq_msk, offset));

            return true;
        }
#endif

        bool emplace(storage_t&& val) noexcept {
            auto h_ = pop_from_list(free_);
            if (h_ == empty_tag) {
                return false;
            }

            auto seq_ = get_seq(h_), offset = get_offset(h_);
            auto& slot = nodes[offset];
            slot.satellite.construct(std::move(val));
            append_to_list(head_, make_seq((seq_ + 1) & seq_msk, offset));

            return true;
        }

        template <typename T_ = storage_t, typename... Args,
                std::enable_if_t<std::is_nothrow_constructible<T_, Args&&...>::value>* = nullptr>
        bool emplace(Args&&... args) noexcept {
            auto h_ = pop_from_list(free_);
            if (h_ == empty_tag) {
                return false;
            }

            auto seq_ = get_seq(h_), offset = get_offset(h_);
            auto& slot = nodes[offset];
            slot.satellite.construct(std::forward<Args>(args)...);
            append_to_list(head_, make_seq((seq_ + 1) & seq_msk, offset));

            return true;
        }

#if LFNDS_HAS_EXCEPTIONS
        template <typename T_ = storage_t, typename... Args,
                std::enable_if_t<conjunction_v<
                        negation<std::is_nothrow_constructible<T_, Args&&...>>,
                        std::is_constructible<T_, Args&&...>>
                >* = nullptr>
        bool emplace(Args&&... args) {
            T_ tmp(std::forward<Args>(args)...);

            auto h_ = pop_from_list(free_);
            if (h_ == empty_tag) {
                return false;
            }

            auto seq_ = get_seq(h_), offset = get_offset(h_);
            auto& slot = nodes[offset];
            slot.satellite.construct(std::move(tmp));
            append_to_list(head_, make_seq((seq_ + 1) & seq_msk, offset));

            return true;
        }
#endif

        inplace_t<storage_t> pop() noexcept {
            auto h_ = pop_from_list(head_);
            if (h_ == empty_tag) {
                return inplace_t<storage_t>();
            }

            auto seq_ = get_seq(h_), offset = get_offset(h_);
            auto seq = make_seq(seq_, offset);
            auto& slot = nodes[offset];
            inplace_t<storage_t> result(std::move(*slot.satellite.ptr()));
            nodes[offset].satellite.destroy();

            append_to_list(free_, seq);
            return result;
        }
    };
}

#endif
