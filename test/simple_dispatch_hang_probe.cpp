#include "executor/simple_executor.h"
int main(){
    flux_foundry::simple_executor<1> ex;
    ex.dispatch(flux_foundry::task_wrapper_sbo([]() noexcept {}));
    ex.shutdown();
    ex.dispatch(flux_foundry::task_wrapper_sbo([]() noexcept {})); // expected to fail-fast; currently spins forever
    return 0;
}
