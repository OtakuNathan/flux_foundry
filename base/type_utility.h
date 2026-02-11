#ifndef FLUX_FOUNDRY_TYPE_UTILITY_H
#define FLUX_FOUNDRY_TYPE_UTILITY_H

#include "traits.h"

namespace flux_foundry {
    // ctor deleter
    template <typename T, bool EnableCopy, bool EnableMove>
    struct ctor_delete_base;

    template <typename T>
    struct ctor_delete_base <T, true, true> {
        ctor_delete_base() = default;
        ctor_delete_base(const ctor_delete_base&) = default;
        ctor_delete_base(ctor_delete_base&&) noexcept = default;
        ctor_delete_base& operator=(const ctor_delete_base&) = default;
        ctor_delete_base& operator=(ctor_delete_base&&) noexcept = default;
    };

    template <typename T>
    struct ctor_delete_base<T, true, false> {
        ctor_delete_base() = default;
        ctor_delete_base(const ctor_delete_base&) = default;
        ctor_delete_base(ctor_delete_base&&) = delete;
        ctor_delete_base& operator=(const ctor_delete_base&) = default;
        ctor_delete_base& operator=(ctor_delete_base&&) noexcept = default;
    };

    template <typename T>
    struct ctor_delete_base<T, false, true> {
        ctor_delete_base() = default;
        ctor_delete_base(const ctor_delete_base&) = delete;
        ctor_delete_base(ctor_delete_base&&) noexcept = default;
        ctor_delete_base& operator=(const ctor_delete_base&) = default;
        ctor_delete_base& operator=(ctor_delete_base&&) noexcept = default;
    };

    template <typename T>
    struct ctor_delete_base<T, false, false> {
        ctor_delete_base() = default;
        ctor_delete_base(const ctor_delete_base&) = delete;
        ctor_delete_base(ctor_delete_base&&) noexcept = delete;
        ctor_delete_base& operator=(const ctor_delete_base&) = default;
        ctor_delete_base& operator=(ctor_delete_base&&) noexcept = default;
    };

    // assign deleter
    template <typename T, bool EnableCopy, bool EnableMove>
    struct assign_delete_base;

    template  <typename T>
    struct assign_delete_base <T, true, true> {
        assign_delete_base() = default;
        assign_delete_base(const assign_delete_base&) = default;
        assign_delete_base(assign_delete_base&&) noexcept = default;
        assign_delete_base& operator=(const assign_delete_base&) = default;
        assign_delete_base& operator=(assign_delete_base&&) noexcept = default;
    };

    template <typename T>
    struct assign_delete_base<T, true, false> {
        assign_delete_base() = default;
        assign_delete_base(const assign_delete_base&) = default;
        assign_delete_base(assign_delete_base&&) noexcept = default;
        assign_delete_base& operator=(const assign_delete_base&) = default;
        assign_delete_base& operator=(assign_delete_base&&) noexcept = delete;
    };

    template <typename T>
    struct assign_delete_base<T, false, true> {
        assign_delete_base() = default;
        assign_delete_base(const assign_delete_base&) = default;
        assign_delete_base(assign_delete_base&&) noexcept = default;
        assign_delete_base& operator=(const assign_delete_base&) = delete;
        assign_delete_base& operator=(assign_delete_base&&) noexcept = default;
    };

    template <typename T>
    struct assign_delete_base<T, false, false> {
        assign_delete_base() = default;
        assign_delete_base(const assign_delete_base&) = default;
        assign_delete_base(assign_delete_base&&) noexcept = default;
        assign_delete_base& operator=(const assign_delete_base&) = delete;
        assign_delete_base& operator=(assign_delete_base&&) = delete;
    };

}


#endif
