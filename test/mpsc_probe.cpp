#include <utility>
#include "utility/concurrent_queues.h"
int main() {
    flux_foundry::mpsc_queue<std::pair<int,int>, 8> q;
    bool ok = q.try_emplace(1, 2);
    return ok ? 0 : 1;
}
