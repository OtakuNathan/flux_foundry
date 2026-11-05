// The application services CUDA completion from its ordinary host loop.
// No FF scheduling policy and no CUDA host callback are involved.
#include <dynabridge/extensions/flux_foundry/async.h>
#include <dynabridge/backend_base.h>
#include "cuda_image_kernel.cuh"
#include "cuda_completion_condition.h"
#include <limits>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <functional>
#include <thread>
#include <vector>
struct image_result {int width,height;std::vector<unsigned char> pixels;};
#define DYNABRIDGE_IMPORT_DEF "cuda_typed.def"
#include <dynabridge/import.h>
#undef DYNABRIDGE_IMPORT_DEF
namespace {
__global__ void typed_add_one(int value,int* output){*output=value+1;}
struct cuda_category final:std::error_category {
    const char* name()const noexcept override{return "cuda";}
    std::string message(int e)const override{return cudaGetErrorString(static_cast<cudaError_t>(e));}
};
dynabridge::async_error cuda_error(cudaError_t e){static cuda_category category;return std::error_code(int(e),category);}
struct resources {
    cudaStream_t stream=nullptr;cudaEvent_t event=nullptr;void* device=nullptr;
    std::vector<unsigned char> host;
    cudaError_t error=cudaSuccess;
    bool recorded=false;
    ~resources(){if(device)cudaFree(device);if(event)cudaEventDestroy(event);if(stream)cudaStreamDestroy(stream);}
};
struct gpu_backend:dynabridge::backend_base<gpu_backend> {
    struct context {
        using backend_t=gpu_backend;
        bool closing=false,fail_record=false;
        std::vector<std::function<bool()>> pending;
        // Owner thread only. Close never joins or waits inside a continuation.
        void close() noexcept {closing=true;}
        void poll(){
            auto round=std::move(pending);pending.clear();
            for(auto& operation:round) if(!operation())pending.emplace_back(std::move(operation));
        }
    };
    template<class R,class Direction> struct object_t{};
    template<class Symbol,class Contract,class Receiver,class Tuple>
    static auto prepare_async(std::shared_ptr<Receiver> receiver,Tuple args);
};
template<class Symbol> struct kernel;
template<> struct kernel<dynabridge::import_symbols::gpu::add_one> {
    using result_type=int;
    static bool valid(const std::tuple<int>& args){return std::get<0>(args)<std::numeric_limits<int>::max();}
    static std::size_t bytes(const std::tuple<int>&){return sizeof(int);}
    static void launch(resources& r,const std::tuple<int>& args){typed_add_one<<<1,1,0,r.stream>>>(std::get<0>(args),static_cast<int*>(r.device));}
    static int collect(resources& r,const std::tuple<int>&){int value;std::memcpy(&value,r.host.data(),sizeof(value));return value;}
};
template<> struct kernel<dynabridge::import_symbols::gpu::render> {
    using result_type=image_result;
    static bool valid(const std::tuple<int,int>& args){return std::get<0>(args)>0&&std::get<0>(args)<=4096&&std::get<1>(args)>0&&std::get<1>(args)<=4096;}
    static std::size_t bytes(const std::tuple<int,int>& args){return std::size_t(std::get<0>(args))*std::get<1>(args)*4;}
    static void launch(resources& r,const std::tuple<int,int>& args){int w=std::get<0>(args),h=std::get<1>(args);mandelbrot_rgba_kernel<<<dim3((w+15)/16,(h+15)/16),dim3(16,16),0,r.stream>>>(static_cast<unsigned char*>(r.device),w,h,1.f);}
    static image_result collect(resources& r,const std::tuple<int,int>& args){return {std::get<0>(args),std::get<1>(args),std::move(r.host)};}
};
template<class Symbol,class Contract,class Receiver,class Tuple>
auto gpu_backend::prepare_async(std::shared_ptr<Receiver> receiver,Tuple args){
    using K=kernel<Symbol>;using R=typename K::result_type;
    return dynabridge::make_invocation<R>(std::move(receiver),[args=std::move(args)](Receiver& receiver,auto done) DYNABRIDGE_NOEXCEPT_IF_DISABLED {
        auto& ctx=receiver.context();
        if(!K::valid(args))return dynabridge::async_submission::reject(cuda_error(cudaErrorInvalidValue));
        if(ctx.closing)return dynabridge::async_submission::reject(cuda_error(cudaErrorNotReady));
        auto state=std::make_shared<resources>();state->host.resize(K::bytes(args));
        // Reserve the completion record before launching any device work.
        ctx.pending.emplace_back([state,args,done]() mutable {
            int error=int(state->error);
            if(!cuda_completion_ready(state->recorded,error,int(cudaSuccess),int(cudaErrorNotReady),
                [&]{return int(cudaEventQuery(state->event));},[&]{return int(cudaStreamSynchronize(state->stream));}))return false;
            state->error=static_cast<cudaError_t>(error);
            if(state->error!=cudaSuccess) {
                const auto error=cuda_error(state->error);state.reset();
                done.finish(dynabridge::async_result<R>(error));
            } else {
                auto result=K::collect(*state,args);state.reset();
                done.finish(dynabridge::async_result<R>(std::move(result)));
            }
            return true;
        });
        auto fail=[&](cudaError_t code){state->error=code;return dynabridge::async_submission::accept();};
        auto code=cudaStreamCreateWithFlags(&state->stream,cudaStreamNonBlocking);if(code!=cudaSuccess){ctx.pending.pop_back();return dynabridge::async_submission::reject(cuda_error(code));}
        code=cudaEventCreateWithFlags(&state->event,cudaEventDisableTiming);if(code!=cudaSuccess)return fail(code);
        code=cudaMalloc(&state->device,state->host.size());if(code!=cudaSuccess)return fail(code);
        K::launch(*state,args);code=cudaGetLastError();if(code!=cudaSuccess)return fail(code);
        code=cudaMemcpyAsync(state->host.data(),state->device,state->host.size(),cudaMemcpyDeviceToHost,state->stream);if(code!=cudaSuccess)return fail(code);
        code=ctx.fail_record?cudaErrorInvalidValue:cudaEventRecord(state->event,state->stream);
        if(code!=cudaSuccess)return fail(code);
        state->recorded=true;return dynabridge::async_submission::accept();
    });
}
}
int main(int argc,char**){
    int devices=0;if(cudaGetDeviceCount(&devices)!=cudaSuccess || !devices){std::puts("SKIP: no CUDA device");return 77;}
    auto ctx=std::make_shared<gpu_backend::context>();ctx->fail_record=argc>1;
    using D=dynabridge::gpu<gpu_backend::context>;
    auto gpu=dynabridge::bind_static_async<dynabridge::gpu>(ctx,D::object_t{});
    int completed=0;bool valid=true;
    dynabridge::ff::start_invocation(gpu.add_one(41),[&](flux_foundry::result_t<int,dynabridge::async_error> r) noexcept {valid&=ctx->fail_record?!r.has_value():(r.has_value()&&r.value()==42);++completed;});
    dynabridge::ff::start_invocation(gpu.render(128,96),[&](flux_foundry::result_t<image_result,dynabridge::async_error> r) noexcept {valid&=ctx->fail_record?!r.has_value():(r.has_value()&&r.value().pixels.size()==128*96*4);++completed;ctx->close();});
    while(!ctx->pending.empty()){ctx->poll();std::this_thread::sleep_for(std::chrono::microseconds(100));}
    return valid&&completed==2?0:1;
}
