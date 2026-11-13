#include "../flow/flow_controller_pool.h"

namespace {
using namespace flux_foundry;
auto make_controller() {
    return make_lite_ptr_with_allocator<flow_controller>(pooled_controller_allocator<>{});
}
}

void controller_pool_header_smoke() {
    auto controller = make_controller();
    if (controller) controller->cancel();
}
