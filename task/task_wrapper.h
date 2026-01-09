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
    // this is not thread safe
    template <size_t sbo_size_, size_t align_>
    class task_wrapper : 
        public raw_type_erase_base<task_wrapper<sbo_size_, align_>, sbo_size_, align_, void(void*)>  {
        using base = raw_type_erase_base<task_wrapper<sbo_size_, align_>, sbo_size_, align_, void(void*)>;

        template <typename T, bool sbo_enabled>
        struct task_vfns {
            static void call(void* p) noexcept  {
                (*tr_ptr<T, sbo_enabled>(p))();
            }
        };

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

        template <typename T, bool sbo_enabled>
        void do_customize() noexcept {
            static_assert(std::is_object<T>::value && !std::is_reference<T>::value,
                "T must be a non-reference object type.");
            static_assert(is_compatible<T>::value,
                "the given type is not compatible with task_wrapper container. T must be void() noexcept.");
            this->invoker_ = task_vfns<T, sbo_enabled>::call;
            this->manager_ = life_span_manager<T, sbo_enabled>::manage;
        }

        task_wrapper() noexcept = default;
        task_wrapper(const task_wrapper&) = delete;
        task_wrapper& operator=(const task_wrapper&) = delete;
        task_wrapper(task_wrapper&&) = default;
        task_wrapper& operator=(task_wrapper&&) = default;
        ~task_wrapper() noexcept = default;

        template <typename U,
            typename T = std::decay_t<U>,
            typename = std::enable_if_t<conjunction_v<negation<is_self_constructing<task_wrapper, T>>, is_compatible<T>>>>
        explicit task_wrapper(U&& rhs) 
            noexcept(noexcept(std::declval<task_wrapper&>().template emplace<T>(std::forward<U>(rhs)))) {
            this->template emplace<T>(std::forward<U>(rhs));
        }

        void operator()() noexcept {
            assert(this->invoker_);
            this->invoker_(this->data_);
        }
    };

    template <size_t _sbo_size, size_t align>
    void swap(task_wrapper<_sbo_size, align>& a, task_wrapper<_sbo_size, align>& b) noexcept {
        a.swap(b);
    }

    using task_wrapper_sbo = task_wrapper<CACHE_LINE_SIZE - 2 * sizeof(std::nullptr_t), alignof(std::max_align_t)>;
    static_assert(sizeof(task_wrapper_sbo) == CACHE_LINE_SIZE,
                  "task_wrapper_sbo must fit exactly in one cache line.");
}

#endif
