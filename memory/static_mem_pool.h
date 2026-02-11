#ifndef FLUX_FOUNDRY_STATIC_MEM_POOL_H
#define FLUX_FOUNDRY_STATIC_MEM_POOL_H

#include "../utility/static_list.h"
#include "../base/traits.h"

namespace flux_foundry {
    template <size_t max_block_count = 16, size_t max_block_size = 512>
    struct static_mem_pool {
        static_assert((max_block_count & (max_block_count - 1)) == 0, 
            "max_block_count must be power of two");

        constexpr static size_t epoch = 4;
        constexpr static size_t maxoff = epoch - 1;
        constexpr static size_t min_block_size = max_block_size >> maxoff;
        constexpr static size_t line_width = max_block_size * max_block_count;

        alignas(std::max_align_t) uint8_t buff[epoch * line_width];

        template <size_t line>
        using list_t = static_list<uint8_t*, (max_block_count << (maxoff - line))>;

        list_t<0> free_0;
        list_t<1> free_1;
        list_t<2> free_2;
        list_t<3> free_3;

        static size_t match(size_t n) noexcept {
            if (n <= (min_block_size << 0)) return 0;
            if (n <= (min_block_size << 1)) return 1;
            if (n <= (min_block_size << 2)) return 2;
            if (n <= (min_block_size << 3)) return 3;
            return 4;
        }

        static size_t block_size(size_t i) noexcept {
            return min_block_size << i;
        }

        ptrdiff_t calc_line(const void* ptr) noexcept {
            auto base = reinterpret_cast<const uint8_t*>(buff);
            auto cur = reinterpret_cast<const uint8_t*>(ptr);
            if (cur < base  || cur >= base + sizeof(buff)) {
                return -1;
            }
            return (cur - base) / line_width;
        }

        bool belong_to(const void* ptr) noexcept {
            return calc_line(ptr) >= 0;
        }

        static_mem_pool() noexcept {
            uint8_t* p = buff;
            for (size_t j = 0; j < (max_block_count << 3); ++j) {
                free_0.emplace(p);
                p += block_size(0);
            }

            for (size_t j = 0; j < (max_block_count << 2); ++j) {
                free_1.emplace(p);
                p += block_size(1);
            }

            for (size_t j = 0; j < (max_block_count << 1); ++j) {
                free_2.emplace(p);
                p += block_size(2);
            }

            for (size_t j = 0; j < (max_block_count << 0); ++j) {
                free_3.emplace(p);
                p += block_size(3);
            }
        }

        void* allocate(size_t n) noexcept {
            inplace_t<uint8_t*> p{};
            /* fallthrough */
            switch (match(n)) {
                case 0: p = free_0.pop(); if (p.has_value()) return p.steal(); 
                case 1: p = free_1.pop(); if (p.has_value()) return p.steal();
                case 2: p = free_2.pop(); if (p.has_value()) return p.steal();
                case 3: p = free_3.pop(); if (p.has_value()) return p.steal();
                default: return nullptr;
            }
        }

        void deallocate(void* ptr) noexcept {
            if (!ptr) {
                return;
            }

            switch (calc_line(ptr)) {
                case 0: free_0.emplace(static_cast<uint8_t*>(ptr)); break;
                case 1: free_1.emplace(static_cast<uint8_t*>(ptr)); break;
                case 2: free_2.emplace(static_cast<uint8_t*>(ptr)); break;
                case 3: free_3.emplace(static_cast<uint8_t*>(ptr)); break;
            }
        }
    };

} // namespace task_system

#endif
