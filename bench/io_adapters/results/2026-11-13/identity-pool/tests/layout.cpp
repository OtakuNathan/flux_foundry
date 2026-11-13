#include "bench/load_probe.h"
int main() {
 std::printf("identity=%d,cache=%d,ordinary=%zu,fast=%zu,identity_bytes=%zu\n",
   FLUX_FOUNDRY_AWAITABLE_IDENTITY_POOL, FLUX_FOUNDRY_AWAITABLE_POOL_SLOT_COUNT,
   sizeof(load_probe::Normal), sizeof(load_probe::Fast), sizeof(std::size_t));
}
