#include "extension/dpdk_awaitable.h"

struct packet {};
struct api {
#if FF_IO_CASE == 1
    // Missing packet_type.
#elif FF_IO_CASE == 2
    using packet_type = void;
#elif FF_IO_CASE == 3
    using packet_type = packet&;
#elif FF_IO_CASE == 4
    using packet_type = const packet;
#else
    using packet_type = packet;
#endif

#if FF_IO_CASE == 5
    // Missing free.
#elif FF_IO_CASE == 6
    static int free(packet*) noexcept { return 0; }
#else
    static void free(packet*) noexcept(FF_IO_CASE != 7) {}
#endif

#if FF_IO_CASE == 8
    // Missing rx.
#elif FF_IO_CASE == 9
    static int rx(std::uint16_t, std::uint16_t, packet**, std::uint16_t) noexcept { return 0; }
#elif FF_IO_CASE == 11
    static std::uint16_t rx(std::uint16_t, std::uint16_t, packet*, std::uint16_t) noexcept { return 0; }
#else
    static std::uint16_t rx(std::uint16_t, std::uint16_t, packet**, std::uint16_t)
        noexcept(FF_IO_CASE != 10) { return 0; }
#endif

#if FF_IO_CASE == 12
    // Missing tx.
#elif FF_IO_CASE == 13
    static int tx(std::uint16_t, std::uint16_t, packet**, std::uint16_t) noexcept { return 0; }
#else
    static std::uint16_t tx(std::uint16_t, std::uint16_t, packet**, std::uint16_t)
        noexcept(FF_IO_CASE != 14) { return 0; }
#endif
};

#if FF_IO_CASE >= 17 && FF_IO_CASE <= 19
struct error {};
namespace flux_foundry { namespace extension { namespace detail {
#if FF_IO_CASE == 18
template <> struct io_error<error> { static int make(int) noexcept { return 0; } };
#elif FF_IO_CASE == 19
template <> struct io_error<error> { static error make(int) noexcept(false) { return {}; } };
#endif
}}}
static_assert(sizeof(flux_foundry::extension::dpdk_rx_awaitable<8, error, api>) > 0,
              "instantiate the error policy contract");
#else
#if FF_IO_CASE == 15
constexpr std::size_t capacity = 0;
#elif FF_IO_CASE == 16
constexpr std::size_t capacity = 65536;
#else
constexpr std::size_t capacity = 8;
#endif
// Context construction must validate its policy even without a poll() call.
static_assert(sizeof(flux_foundry::extension::dpdk_rx_context<capacity, api>) > 0,
              "instantiate the batch/API contracts at the context boundary");
#endif
