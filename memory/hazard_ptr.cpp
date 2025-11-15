
// hazard_ptr.cpp
#include "hazard_ptr.h"

namespace lite_fnds {
	hp_mgr::hazard_record hp_mgr::record[max_slot]{};
#ifdef USE_HEAP_ALLOCATED
	std::atomic<hp_mgr::retire_list_node*> hp_mgr::retire_list(nullptr);
#else
    static static_list<hp_mgr::retire_list_node, hp_mgr::max_slot> retire_list;
#endif
}