#include "utility/back_off.h"

int main() {
    flux_foundry::backoff_strategy<> backoff;
    backoff.reset();
    return 0;
}
