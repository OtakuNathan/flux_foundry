#pragma once
// Perf-only harness hooks. Warmup is the first measure() call, workload second.
// Control/ack pipes are created by perf_probe.py and inherited through perf.
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
namespace probe_perf {
inline void command(const char* text) {
    const int control = std::atoi(std::getenv("FF_PERF_CONTROL_FD"));
    const int ack = std::atoi(std::getenv("FF_PERF_ACK_FD"));
    const auto size = std::strlen(text);
    if (::write(control, text, size) != static_cast<ssize_t>(size)) std::abort();
    char buffer[16];
    ssize_t n;
    do { n = ::read(ack, buffer, sizeof(buffer)); } while (n < 0 && errno == EINTR);
    if (n < 4 || std::memcmp(buffer, "ack\n", 4) != 0) std::abort();
}
inline unsigned& phase() { static unsigned n = 0; return n; }
inline void begin() { if (++phase() == 2) command("enable\n"); }
inline void end() { if (phase() == 2) command("disable\n"); }
} // namespace probe_perf
