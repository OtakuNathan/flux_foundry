#include "memory/static_mem_pool.h"
int main(){ flux_foundry::static_mem_pool<> p; auto* a = p.allocate(10); p.deallocate(a); }
