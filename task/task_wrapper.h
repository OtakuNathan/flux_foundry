//
// Created by nathan on 2025/8/13.
//

#ifndef LITE_FNDS_TASK_WRAPPER_H
#define LITE_FNDS_TASK_WRAPPER_H

#include <cassert>
#include <cstddef>
#include <utility>
#include "../base/traits.h"
#include "../base/inplace_base.h"
#include "../base/type_erase_base.h"

namespace lite_fnds {
    namespace task_handle_impl {
        struct task_vtable : basic_vtable {
            void (*run)(void *) noexcept;

            constexpr task_vtable(
                fn_copy_construct_t *fcopy,
                fn_move_construct_t *fmove,
                fn_safe_relocate_t* fsafe_reloc,
                fn_destroy_t *fdestroy,
                void (*frun)(void *) noexcept) noexcept :
                basic_vtable{fcopy, fmove, fsafe_reloc, fdestroy},
                run(frun) {
            }
        };

        template <typename T, bool sbo_enabled>
        struct task_vfns {
            static void run(void* p) noexcept {
                (*tr_ptr<T, sbo_enabled>(p))();
            }

            static const basic_vtable* table_for() noexcept {
                static constexpr task_vtable vt {
                    fcopy_construct<T, sbo_enabled>(),
                    fmove_construct<T, sbo_enabled>(),
                    fsafe_relocate<T, sbo_enabled>(),
                    fdestroy<T, sbo_enabled>(),
                    &task_vfns::run
                };
                return &vt;
            }
        };
    }

    // this is not thread safe
    template <size_t sbo_size_, size_t align_>
    class task_wrapper : public raw_type_erase_base<task_wrapper<sbo_size_, align_>, sbo_size_, align_>  {
        using base = raw_type_erase_base<task_wrapper<sbo_size_, align_>, sbo_size_, align_>;

        template <class F>
        struct is_compatible {
        private:
            template <typename C>
            static auto test(int) -> conjunction<
                std::is_nothrow_move_constructible<C>,
                std::is_void<decltype(std::declval<C>()())>,
                std::integral_constant<bool, noexcept(std::declval<C>()())>
            >;

            template <typename ...> static auto test(...) -> std::false_type;

        public:
            constexpr static bool value = decltype(test<F>(0))::value;
        };
    public:
        static constexpr size_t sbo_size = sbo_size_;
        static constexpr size_t align = align_;

        template <typename T, bool sbo_enable>
        void fill_vtable() noexcept {
            static_assert(std::is_object<T>::value && !std::is_reference<T>::value,
                "T must be a non-reference object type.");
            static_assert(is_compatible<T>::value,
                "the given type is not compatible with task_wrapper container. T must be void() noexcept.");

            this->_vtable = task_handle_impl::task_vfns<T, sbo_enable>::table_for();
        }

        task_wrapper() noexcept = default;
        task_wrapper(const task_wrapper&) = delete;
        task_wrapper& operator=(const task_wrapper&) = delete;
        ~task_wrapper() noexcept = default;

        template <typename U,
            typename T = std::decay_t<U>,
            typename = std::enable_if_t<conjunction_v<negation<is_self_constructing<task_wrapper, T>>, is_compatible<T>>>>
        explicit task_wrapper(U&& rhs) 
            noexcept(noexcept(std::declval<task_wrapper&>().template emplace<T>(std::forward<U>(rhs)))) {
            this->template emplace<T>(std::forward<U>(rhs));
        }

        using base::emplace;
        using base::swap;
        using base::clear;

        bool empty() const noexcept {
            return !this->has_value();
        }

        task_wrapper(task_wrapper&& rhs) noexcept : base() {
            if (rhs._vtable) {
                // use safe_relocate: always noexcept
                rhs._vtable->safe_relocate(this->_data, rhs._data);

                this->_vtable = rhs._vtable;
                rhs._vtable = nullptr;
            } else {
                this->_vtable = nullptr;
            }
        }

        task_wrapper& operator=(task_wrapper&& rhs) noexcept {
            if (this != &rhs) {
                // clear current
                this->clear();
                if (rhs._vtable) {
                    rhs._vtable->safe_relocate(this->_data, rhs._data);
                    this->_vtable = rhs._vtable;
                    rhs._vtable = nullptr;
                } else {
                    this->_vtable = nullptr;
                }
            }
            return *this;
        }

        void operator()() noexcept {
            assert(this->_vtable);
            static_cast<const task_handle_impl::task_vtable*>(this->_vtable)->run(this->_data);
        }
    };

    template <size_t _sbo_size, size_t align>
    void swap(task_wrapper<_sbo_size, align>& a, task_wrapper<_sbo_size, align>& b) noexcept {
        a.swap(b);
    }

    using task_wrapper_sbo = task_wrapper<CACHE_LINE_SIZE - sizeof(std::nullptr_t), alignof(std::max_align_t)>;
    static_assert(sizeof(task_wrapper_sbo) == CACHE_LINE_SIZE,
                  "task_wrapper_sbo must fit exactly in one cache line.");
}

#endif //__TASK_WRAPPER_H__
