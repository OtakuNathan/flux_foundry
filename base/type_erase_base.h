#ifndef LITE_FNDS_TYPE_ERASE_BASE_H
#define LITE_FNDS_TYPE_ERASE_BASE_H

#include <memory>
#include <new>
#include <cstring>
#include <stdexcept>
#include "inplace_base.h"

/**
 * This class is designed to serve as a base for type erasure facilities using CRTP.
 * * Requirements for the Derived class:
 * 1. Must define a static `stub` function. This is used to initialize `invoker_`
 * (which stores the hot-path function pointer) representing an empty/uninitialized state.
 * 2. Must implement a `do_customize` template function. This function configures the
 * vtable and invoker for a specific concrete type T, and it must be `noexcept`.
 */

namespace lite_fnds {
    constexpr static size_t sbo_size = 64;

    namespace detail {
        template <typename T, size_t sbo_size, size_t align>
        struct can_enable_sbo : conjunction<
            std::integral_constant<bool, (sizeof(T) <= sbo_size) && (alignof(T) <= align)>,
            std::is_nothrow_move_constructible<T>> {};
    }

    template <typename T, bool sbo_enabled, std::enable_if_t<sbo_enabled>* = nullptr>
    constexpr T* tr_ptr(void* addr) noexcept {
        return static_cast<T*>(addr);
    }

    template <typename T, bool sbo_enabled, std::enable_if_t<!sbo_enabled>* = nullptr>
    constexpr T* tr_ptr(void* addr) noexcept {
        return *static_cast<T**>(addr);
    }

    template <typename T, bool sbo_enabled, std::enable_if_t<sbo_enabled>* = nullptr>
    constexpr const T* tr_ptr(const void* addr) noexcept {
        return static_cast<const T*>(addr);
    }

    template <typename T, bool sbo_enabled, std::enable_if_t<!sbo_enabled>* = nullptr>
    constexpr const T* tr_ptr(const void* addr) noexcept {
        return *static_cast<T* const*>(addr);
    }

    enum class type_erase_lifespan_op : uint32_t {
        copy,
        move,
        destroy,
    };

    enum class lifespan_op_error : uint32_t {
        success,
        unsupported,
    };

    template <typename T, bool sbo_enabled,
        std::enable_if_t<!std::is_copy_constructible<T>::value>* = nullptr>
    lifespan_op_error copy_construct_impl(void* dst, const void* src) {
        return lifespan_op_error::unsupported;
    }

    template <typename T, bool sbo_enabled,
        std::enable_if_t<conjunction_v<
            std::integral_constant<bool, sbo_enabled>,
            std::is_trivially_copy_constructible<T>
        >>* = nullptr>
    lifespan_op_error copy_construct_impl(void* dst, const void* src) {
        memcpy(dst, src, sizeof(T));
        return lifespan_op_error::success;
    }

    template <typename T, bool sbo_enabled,
            std::enable_if_t<conjunction_v<std::integral_constant<bool, sbo_enabled>,
            negation<std::is_trivially_copy_constructible<T>>,
            std::is_copy_constructible<T>>>* = nullptr>
    lifespan_op_error copy_construct_impl(void* dst, const void* src) {
        ::new (dst) T(*tr_ptr<T, sbo_enabled>(src));
        return lifespan_op_error::success;
    }

    template <typename T, bool sbo_enabled,
        std::enable_if_t<conjunction_v<std::integral_constant<bool, !sbo_enabled>,
        std::is_copy_constructible<T>>>* = nullptr>
    lifespan_op_error copy_construct_impl(void* dst, const void* src) {
        *static_cast<T**>(dst) = new T(*tr_ptr<T, sbo_enabled>(src));
        return lifespan_op_error::success;
    }

    template <typename T, bool sbo_enabled,
        std::enable_if_t<conjunction_v<
            std::integral_constant<bool, sbo_enabled>,
            std::is_trivially_move_constructible<T>
        >>* = nullptr>
    lifespan_op_error move_construct_impl(void* dst, void* src) {
        memcpy(dst, src, sizeof(T));
        return lifespan_op_error::success;
    }


    template <typename T, bool sbo_enabled,
        std::enable_if_t<conjunction_v<
            std::integral_constant<bool, sbo_enabled>,
            negation<std::is_trivially_move_constructible<T>>,
            std::is_move_constructible<T>>
        >* = nullptr>
    lifespan_op_error move_construct_impl(void* dst, void* src) {
        new (dst) T(std::move(*tr_ptr<T, sbo_enabled>(src)));
        return lifespan_op_error::success;
    }

    template <typename T, bool sbo_enabled,
        std::enable_if_t<!sbo_enabled>* = nullptr>
    lifespan_op_error move_construct_impl(void* dst, void* src) {
        T*& src_ptr = *static_cast<T**>(src);
        *static_cast<T**>(dst) = src_ptr;
        src_ptr = nullptr;
        return lifespan_op_error::success;
    }

    template <typename T, bool sbo_enabled,
        std::enable_if_t<sbo_enabled>* = nullptr>
    lifespan_op_error destroy_impl(void* addr) noexcept {
        tr_ptr<T, sbo_enabled>(addr)->~T();
        return lifespan_op_error::success;
    }

    template <typename T, bool sbo_enabled,
        std::enable_if_t<!sbo_enabled>* = nullptr>
    lifespan_op_error destroy_impl(void* addr) noexcept {
        T*& p = *static_cast<T**>(addr);
        delete p;
        p = nullptr;
        return lifespan_op_error::success;
    }

    template <typename T, bool enabled>
    struct life_span_manager {
        static lifespan_op_error manage(void* dst, const void* src, type_erase_lifespan_op op) {
            switch (op) {
            case type_erase_lifespan_op::copy:
                return copy_construct_impl<T, enabled>(dst, src);
            case type_erase_lifespan_op::move:
                return move_construct_impl<T, enabled>(dst, const_cast<void*>(src));
            case type_erase_lifespan_op::destroy:
                return destroy_impl<T, enabled>(const_cast<void*>(src));
            default:
                return lifespan_op_error::unsupported;
            }
        }
    };

    using lite_span_management = lifespan_op_error(void* dst, const void* src, type_erase_lifespan_op op);

    template <typename derived,
        size_t size = sbo_size,
        size_t align = alignof(std::max_align_t),
        typename hotpath_invoker_t = void>
    struct raw_type_erase_base {
        static_assert(sizeof(void*) <= size, "the given buffer should be at least sufficient to store a T*");
        hotpath_invoker_t* invoker_;
        lite_span_management* manager_;
        alignas(align) unsigned char data_[size];

        static constexpr size_t buf_size = size;

        raw_type_erase_base() noexcept
            : invoker_{ derived::stub }, manager_ { nullptr } {
        }


        raw_type_erase_base(const raw_type_erase_base& rhs)
#if !LFNDS_COMPILER_HAS_EXCEPTIONS
        noexcept
#endif
            : invoker_ { derived::stub } , manager_ { nullptr } {
            if (rhs.manager_) {
                auto res = rhs.manager_(this->data_, rhs.data_, type_erase_lifespan_op::copy);
                if (res != lifespan_op_error::success) {
#if LFNDS_COMPILER_HAS_EXCEPTIONS
                    throw std::runtime_error("the object is not copy constructible");
#else
                    assert(false && "the object is not copy constructible");
                    std::abort();
#endif
                }
                this->manager_ = rhs.manager_;
                this->invoker_ = rhs.invoker_;
            }
        }

        raw_type_erase_base& operator=(const raw_type_erase_base& rhs)
#if !LFNDS_COMPILER_HAS_EXCEPTIONS
        noexcept
#endif
        {
            if (this == &rhs) {
                return *this;
            }
            raw_type_erase_base tmp(rhs);
            this->swap(tmp);
            return *this;
        }

        raw_type_erase_base(raw_type_erase_base&& rhs) noexcept
            : invoker_ { derived::stub } , manager_ { nullptr } {
            if (rhs.manager_) {
                rhs.manager_(this->data_, rhs.data_, type_erase_lifespan_op::move);
                this->invoker_ = rhs.invoker_;
                this->manager_ = rhs.manager_;
                rhs.clear();
            }
        }

        raw_type_erase_base& operator=(raw_type_erase_base&& rhs) noexcept {
            if (this == &rhs) {
                return *this;
            }
            raw_type_erase_base tmp(std::move(rhs));
            this->swap(tmp);
            return *this;
        }

        explicit operator bool() const noexcept {
            return this->manager_ != nullptr;
        }

        template <typename U, typename T = std::decay_t<U>, typename ... Args,
            std::enable_if_t<conjunction_v<
            std::is_nothrow_constructible<T, Args&&...>,
            detail::can_enable_sbo<T, buf_size, align>>>* = nullptr>
        void emplace(Args &&... args) noexcept {
            static_assert(align >= alignof(T), "SBO placement-new requires buffer alignment >= alignof(T)");
            this->clear();
            new (data_) T(std::forward<Args>(args)...);
            auto derived_ = static_cast<derived*>(this);
            derived_->template do_customize<T, true>();
            assert(this->manager_ != nullptr && "Derived class failed to set manager in do_customize!");
        }

#if LFNDS_HAS_EXCEPTIONS
        template <typename U, typename T = std::decay_t<U>, typename... Args,
            std::enable_if_t<conjunction_v<
                std::is_constructible<T, Args&&...>,
                negation<std::is_nothrow_constructible<T, Args&&...>>,
                detail::can_enable_sbo<T, buf_size, align>>>* = nullptr>
        void emplace(Args &&... args)
            noexcept(std::is_nothrow_constructible<T, Args &&...>::value) {
            static_assert(align >= alignof(T), "SBO placement-new requires buffer alignment >= alignof(T)");
            T tmp(std::forward<Args>(args)...);
            this->clear();

            new (data_) T(std::move(tmp));
            auto derived_ = static_cast<derived*>(this);
            derived_->template do_customize<T, true>();
            assert(this->manager_ != nullptr && "Derived class failed to set manager in do_customize!");
        }
#endif

        template <typename U, typename T = std::decay_t<U>, typename... Args,
            std::enable_if_t<conjunction_v<
#if LFNDS_HAS_EXCEPTIONS
                std::is_constructible<T, Args &&...>,
#else
                std::is_nothrow_constructible<T, Args&&...>,
#endif
                negation<detail::can_enable_sbo<T, buf_size, align>>>> * = nullptr >
            void emplace(Args &&... args) {
            static_assert(align >= alignof(T*),
                "SBO placement-new requires buffer alignment >= alignof(T*)");

            std::unique_ptr<T> tmp(new
#if !LFNDS_COMPILER_HAS_EXCEPTIONS
            (std::nothrow)
#endif
                T(std::forward<Args>(args)...));
#if !LFNDS_HAS_EXCEPTIONS
            if (!tmp) {
                return;
            }
#endif
            this->clear();

            *reinterpret_cast<T**>(data_) = tmp.release();
            auto derived_ = static_cast<derived*>(this);
            derived_->template do_customize<T, false>();
            assert(this->manager_ != nullptr && "Derived class failed to set manager in do_customize!");
        }

        void swap(raw_type_erase_base& rhs) noexcept {
            if (this == &rhs || (!this->manager_ && !rhs.manager_)) {
                return;
            }

            LIKELY_IF (this->manager_ && rhs.manager_) {
                alignas(align) unsigned char backup[buf_size];

                this->manager_(backup, this->data_, type_erase_lifespan_op::move);
                this->manager_(nullptr, this->data_, type_erase_lifespan_op::destroy);

                rhs.manager_(this->data_, rhs.data_, type_erase_lifespan_op::move);
                rhs.manager_(nullptr, rhs.data_, type_erase_lifespan_op::destroy);

                this->manager_(rhs.data_, backup, type_erase_lifespan_op::move);
                this->manager_(nullptr, backup, type_erase_lifespan_op::destroy);
            } else {
                if (this->manager_ && !rhs.manager_) {
                    this->manager_(rhs.data_, this->data_, type_erase_lifespan_op::move);
                    this->manager_(nullptr, this->data_, type_erase_lifespan_op::destroy);
                } else {
                    rhs.manager_(this->data_, rhs.data_, type_erase_lifespan_op::move);
                    rhs.manager_(nullptr, rhs.data_, type_erase_lifespan_op::destroy);
                }
            }

            using std::swap;
            swap(this->invoker_, rhs.invoker_);
            swap(this->manager_, rhs.manager_);
        }

        void clear() noexcept {
            if (manager_) {
                manager_(nullptr, data_, type_erase_lifespan_op::destroy);
                manager_ = nullptr;
            }
            invoker_ = derived::stub;
        }

        ~raw_type_erase_base() noexcept {
            clear();
        }
    };
}

#endif
