#ifndef LITE_FNDS_TYPE_ERASE_BASE_H
#define LITE_FNDS_TYPE_ERASE_BASE_H

#include <memory>
#include <new>
#include <stdexcept>
#include "inplace_base.h"

/**
* This class is designed to be used as a base of type_erasing tools,
* To properly use this, you have to make your vtable ONLY inherit basic_vtable,
* besides, you have to provide your fill_vtable 
* template function and make it noexcept (and it should be) 
*/

namespace lite_fnds {
    constexpr static size_t sbo_size = 64;

    using fn_copy_construct_t = void(void *dst, const void *src);
    using fn_move_construct_t = void(void *dst, void *src);
    using fn_safe_relocate_t = void(void *dst, void *src);
    using fn_destroy_t = void(void *dst);

    template <typename T, bool sbo_enabled, std::enable_if_t<sbo_enabled>* = nullptr>
    constexpr T *tr_ptr(void *addr) noexcept {
        return static_cast<T *>(addr);
    }

    template <typename T, bool sbo_enabled, std::enable_if_t<!sbo_enabled>* = nullptr>
    constexpr T *tr_ptr(void *addr) noexcept {
        return *static_cast<T **>(addr);
    }

    template <typename T, bool sbo_enabled, std::enable_if_t<sbo_enabled>* = nullptr>
    constexpr const T *tr_ptr(const void *addr) noexcept {
        return static_cast<const T *>(addr);
    }

    template <typename T, bool sbo_enabled, std::enable_if_t<!sbo_enabled>* = nullptr>
    constexpr const T *tr_ptr(const void *addr) noexcept {
        return *static_cast<T * const*>(addr);
    }

    template <typename T, bool sbo_enabled,
        std::enable_if_t<negation<std::is_copy_constructible<T> >::value>* = nullptr>
    constexpr fn_copy_construct_t *fcopy_construct() noexcept {
        return nullptr;
    }

    template <typename T, bool sbo_enabled,
        std::enable_if_t<sbo_enabled && std::is_copy_constructible<T>::value>* = nullptr>
    constexpr fn_copy_construct_t *fcopy_construct() noexcept {
        return +[](void *dst, const void *src) {
            ::new (dst) T(*tr_ptr<T, sbo_enabled>(src));
        };
    }

    template <typename T, bool sbo_enabled,
        std::enable_if_t<!sbo_enabled && std::is_copy_constructible<T>::value>* = nullptr>
    constexpr fn_copy_construct_t *fcopy_construct() noexcept {
        return +[](void *dst, const void *src) {
            *static_cast<T **>(dst) = new T(*tr_ptr<T, sbo_enabled>(src));
        };
    }

    template <typename T, bool sbo_enabled,
        std::enable_if_t<sbo_enabled && negation<std::is_move_constructible<T>>::value>* = nullptr>
    constexpr fn_move_construct_t *fmove_construct() noexcept {
        return nullptr;
    }

    template <typename T, bool sbo_enabled,
        std::enable_if_t<sbo_enabled && std::is_move_constructible<T>::value >* = nullptr>
    constexpr fn_move_construct_t *fmove_construct() noexcept {
        return +[](void *dst, void *src) {
            new(dst) T(std::move(*tr_ptr<T, sbo_enabled>(src)));
        };
    }

    template <typename T, bool sbo_enabled,
        std::enable_if_t<!sbo_enabled>* = nullptr>
    constexpr fn_move_construct_t *fmove_construct() noexcept {
        return +[](void *dst, void *src) {
            T*& src_ptr = *static_cast<T**>(src);
            *static_cast<T **>(dst) = src_ptr;
            src_ptr = nullptr;
        };
    }

    template <typename T, bool sbo_enabled, std::enable_if_t<!sbo_enabled>* = nullptr>
    constexpr fn_safe_relocate_t *fsafe_relocate() noexcept {
        return +[](void *dst, void *src) noexcept  {
            T*& src_ptr = *static_cast<T**>(src);
            *static_cast<T **>(dst) = src_ptr;
            src_ptr = nullptr;
        };
    }

    template <typename T, bool sbo_enabled,
        std::enable_if_t<sbo_enabled && std::is_nothrow_move_constructible<T>::value>* = nullptr>
    constexpr fn_safe_relocate_t *fsafe_relocate() noexcept {
        return +[](void *dst, void *src) noexcept {
            auto _src = tr_ptr<T, sbo_enabled>(src);
            ::new (dst) T(std::move(*_src));
            _src->~T();
        };
    }

    template <typename T, bool sbo_enabled,
        std::enable_if_t <sbo_enabled
            && !std::is_nothrow_move_constructible<T>::value
            && std::is_nothrow_copy_constructible<T>::value >* = nullptr>
    constexpr fn_safe_relocate_t *fsafe_relocate() noexcept {
        return +[](void *dst, void *src) noexcept {
            auto _src = tr_ptr<T, sbo_enabled>(src);
            ::new (dst) T(*_src);
            _src->~T();
        };
    }

    template <typename T, bool sbo_enabled,
        std::enable_if_t<sbo_enabled>* = nullptr>
    constexpr fn_destroy_t *fdestroy() noexcept {
        return +[](void *addr) noexcept {
            tr_ptr<T, sbo_enabled>(addr)->~T();
        };
    }

    template <typename T, bool sbo_enabled,
        std::enable_if_t<!sbo_enabled>* = nullptr>
    constexpr fn_destroy_t *fdestroy() noexcept {
        return +[](void *addr) noexcept {
            delete tr_ptr<T, sbo_enabled>(addr);
        };
    }

    struct basic_vtable {
        fn_copy_construct_t *copy_construct;
        fn_move_construct_t *move_construct;
        fn_safe_relocate_t  *safe_relocate;
        fn_destroy_t *destroy;
    };

    template <typename derived, 
        size_t size = sbo_size, 
        size_t align = alignof(std::max_align_t)>
    struct raw_type_erase_base {
        static_assert(sizeof(void *) <= size, "the given buffer should be at least sufficient to store a T*");
        alignas(align) unsigned char _data[size];
        const basic_vtable *_vtable;

        static constexpr size_t buf_size = size;

        raw_type_erase_base() noexcept 
            : _vtable{nullptr} {
        }

#if LFNDS_COMPILER_HAS_EXCEPTIONS
        raw_type_erase_base(const raw_type_erase_base& rhs)
            : _vtable(nullptr) {
            if (rhs._vtable) {
                if (!rhs._vtable->copy_construct) {
                    throw std::runtime_error("the object is not copy constructible");
                }

                rhs._vtable->copy_construct(this->_data, rhs._data);
                this->_vtable = rhs._vtable;
            }
        }

        raw_type_erase_base(raw_type_erase_base&& rhs)
            : _vtable(nullptr) {
            if (rhs._vtable) {
                if (!rhs._vtable->move_construct) {
                    throw std::runtime_error("the object is not move constructible");
                }

                rhs._vtable->move_construct(this->_data, rhs._data);
                this->_vtable = rhs._vtable;
                rhs.clear();
            }
        }

        raw_type_erase_base& operator=(const raw_type_erase_base &rhs) {
            if (this == &rhs) {
                return *this;
            }
            raw_type_erase_base tmp(rhs);
            this->swap(tmp);
            return *this;
        }

        raw_type_erase_base& operator=(raw_type_erase_base&& rhs) {
            if (this == &rhs) {
                return *this;
            }
            raw_type_erase_base tmp(std::move(rhs));
            this->swap(tmp);
            return *this;
        }
#else
        raw_type_erase_base(const raw_type_erase_base& rhs) = delete;
        raw_type_erase_base(raw_type_erase_base&& rhs) = delete;
        raw_type_erase_base& operator=(const raw_type_erase_base& rhs) = delete;
        raw_type_erase_base& operator=(raw_type_erase_base&& rhs) = delete;
#endif //  LFNDS_HAS_EXCEPTIONS

        bool has_value() const noexcept {
            return this->_vtable != nullptr;
        }

        explicit operator bool() const noexcept {
            return this->has_value();
        }

        template <typename U, typename T = std::decay_t<U>, typename... Args,
            std::enable_if_t<conjunction_v<
                std::is_nothrow_constructible<T, Args&&...>,
                std::integral_constant<bool, sizeof(T) <= buf_size>,
                can_strong_move_or_copy_constructible<T>>>* = nullptr>
        void emplace(Args &&... args) noexcept {
            static_assert(align >= alignof(T), "SBO placement-new requires buffer alignment >= alignof(T)");
            if (_vtable) {
                _vtable->destroy(_data);
                _vtable = nullptr;
            }
            new(_data) T(std::forward<Args>(args)...);
            auto derived_ = static_cast<derived *>(this);
            derived_->template fill_vtable<T, true>();
        }

#if LFNDS_HAS_EXCEPTIONS
        template <typename U, typename T = std::decay_t<U>, typename... Args,
            std::enable_if_t<conjunction_v<
                std::is_constructible<T, Args &&...>,
                std::integral_constant<bool, sizeof(T) <= buf_size>,
                negation<std::is_nothrow_constructible<T, Args &&...> >,
                std::is_nothrow_move_constructible<T> > >* = nullptr>
        void emplace(Args &&... args)
            noexcept(std::is_nothrow_constructible<T, Args &&...>::value) {
            static_assert(align >= alignof(T), "SBO placement-new requires buffer alignment >= alignof(T)");
            T tmp(std::forward<Args>(args)...);
            if (_vtable) {
                _vtable->destroy(_data);
                _vtable = nullptr;
            }

            new (_data) T(std::move(tmp));
            auto derived_ = static_cast<derived *>(this);
            derived_->template fill_vtable<T, true>();
        }

        template <typename U, typename T = std::decay_t<U>, typename... Args,
            std::enable_if_t<conjunction_v<
                std::is_constructible<T, Args &&...>,
                std::integral_constant<bool, sizeof(T) <= buf_size>,
                negation<std::is_nothrow_constructible<T, Args &&...> >,
                negation<std::is_nothrow_move_constructible<T> >,
                std::is_nothrow_copy_constructible<T>
            > >* = nullptr>
        void emplace(Args &&... args) 
            noexcept(std::is_nothrow_constructible<T, Args &&...>::value) {
            static_assert(align >= alignof(T), 
                "SBO placement-new requires buffer alignment >= alignof(T)");
            T tmp(std::forward<Args>(args)...);
            if (_vtable) {
                _vtable->destroy(_data);
                _vtable = nullptr;
            }

            new (_data) T(tmp);
            auto derived_ = static_cast<derived *>(this);
            derived_->template fill_vtable<T, true>();
        }
#endif

        template <typename U, typename T = std::decay_t<U>, typename... Args,
            std::enable_if_t<conjunction_v<
#if LFNDS_HAS_EXCEPTIONS
                std::is_constructible<T, Args &&...>,
#else
                std::is_nothrow_constructible<T, Args&&...>,
#endif
                disjunction<std::integral_constant<bool, !(sizeof(T) <= buf_size)>, 
                            negation<can_strong_move_or_copy_constructible<T>>>>>* = nullptr>
        void emplace(Args &&... args) {
            static_assert(align >= alignof(T*), 
                "SBO placement-new requires buffer alignment >= alignof(T*)");

            std::unique_ptr<T> tmp = std::make_unique<T>(std::forward<Args>(args)...);
#if !LFNDS_HAS_EXCEPTIONS
            if (!tmp) {
                return;
            }
#endif
            if (_vtable) {
                _vtable->destroy(_data);
                _vtable = nullptr;
            }

            *reinterpret_cast<T **>(_data) = tmp.release();
            auto derived_ = static_cast<derived *>(this);
            derived_->template fill_vtable<T, false>();
        }

        void swap(raw_type_erase_base& rhs) noexcept {
            if (this == &rhs || (!this->_vtable && !rhs._vtable)) {
                return;
            }

            if (this->_vtable && !rhs._vtable) {
                this->_vtable->safe_relocate(rhs._data, this->_data);
                rhs._vtable = this->_vtable;
                this->_vtable = nullptr;
                return;
            }

            if (!this->_vtable && rhs._vtable) {
                rhs._vtable->safe_relocate(this->_data, rhs._data);
                this->_vtable = rhs._vtable;
                rhs._vtable = nullptr;
                return;
            }

            alignas(align) unsigned char backup[buf_size];
            this->_vtable->safe_relocate(backup, this->_data);
            rhs._vtable->safe_relocate(this->_data, rhs._data);
            this->_vtable->safe_relocate(rhs._data, backup);

            using std::swap;
            swap(this->_vtable, rhs._vtable);
        }

        void clear() noexcept {
            if (_vtable) {
                _vtable->destroy(_data);
            }
            _vtable = nullptr;
        }

        ~raw_type_erase_base() noexcept {
            clear();
        }
    };
}

#endif
