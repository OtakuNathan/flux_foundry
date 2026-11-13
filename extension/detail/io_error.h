#ifndef FLUX_FOUNDRY_EXTENSION_IO_ERROR_H
#define FLUX_FOUNDRY_EXTENSION_IO_ERROR_H
#include <exception>
#include <system_error>
#include <type_traits>
#include <utility>
#include "../../base/traits.h"
namespace flux_foundry { namespace extension { namespace detail {
template <typename E> struct io_error;
template <> struct io_error<std::error_code> {
    static std::error_code make(int code) noexcept { return {code, std::generic_category()}; }
};
#if FLUX_FOUNDRY_COMPILER_HAS_EXCEPTIONS
template <> struct io_error<std::exception_ptr> {
    static std::exception_ptr make(int code) noexcept {
        try { return std::make_exception_ptr(std::system_error(code, std::generic_category())); }
        catch (...) { return std::current_exception(); }
    }
};
#endif
template <typename E, typename = void>
struct io_error_prob : std::false_type {};
template <typename E>
struct io_error_prob<E, void_t<decltype(io_error<E>::make(std::declval<int>()))>>
    : conjunction<std::is_same<decltype(io_error<E>::make(std::declval<int>())), E>,
          std::integral_constant<bool, noexcept(io_error<E>::make(std::declval<int>()))>> {};
}}}
#endif
