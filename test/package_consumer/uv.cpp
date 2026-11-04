#include <executor/uv_executor.h>
#include <cstdlib>
int main() {
    uv_loop_t loop{};
    if(uv_loop_init(&loop)) return 1;
    {
        flux_foundry::uv_executor executor(std::nothrow,&loop,4,1);
        if(executor.error()) return 2;
        int calls=0;
        executor.dispatch(flux_foundry::task_wrapper_sbo([&]() noexcept {++calls;}));
        executor.close();
        uv_run(&loop,UV_RUN_DEFAULT);
        if(calls!=1 || !executor.closed()) std::abort();
    }
    return uv_loop_close(&loop)!=0;
}
