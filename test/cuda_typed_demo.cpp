// The application services CUDA completion from its ordinary host loop.
// No FF scheduling policy and no CUDA host callback are involved.
#include <dynabridge/extensions/flux_foundry/async.h>
#include <dynabridge/backend_base.h>
#include "cuda_typed_device.h"
#include "cuda_completion_condition.h"
#include <limits>
#include <list>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <functional>
#include <thread>
#include <vector>
#define DYNABRIDGE_IMPORT_DEF "cuda_typed.def"
#include <dynabridge/import.h>
#undef DYNABRIDGE_IMPORT_DEF
namespace {
struct cuda_category final:std::error_category {
    const char* name()const noexcept override{return "cuda";}
    std::string message(int e)const override{return cudaGetErrorString(static_cast<cudaError_t>(e));}
};
dynabridge::async_error cuda_error(cudaError_t e) noexcept {static cuda_category category;return std::error_code(int(e),category);}
using resources=typed_cuda::resources;
struct gpu_backend:dynabridge::backend_base<gpu_backend> {
    struct context : std::enable_shared_from_this<context> {
        using backend_t=gpu_backend;
        bool closing=false,fail_record=false;
        std::list<std::function<bool()>> pending;
        // Owner thread only. Close never joins or waits inside a continuation.
        void close() noexcept {closing=true;}
        void poll(){
            auto keep_alive=shared_from_this();
            std::list<std::function<bool()>> round;
            round.splice(round.end(),pending);
            while(!round.empty()) {
                auto current=round.begin();
                if((*current)()) round.erase(current);
                else pending.splice(pending.end(),round,current);
            }
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
    static void launch(resources& r,const std::tuple<int>& args){typed_cuda::launch_add_one(r,std::get<0>(args));}
    static int collect(resources& r,const std::tuple<int>&){int value;std::memcpy(&value,r.host.data(),sizeof(value));return value;}
};
template<> struct kernel<dynabridge::import_symbols::gpu::render> {
    using result_type=image_result;
    static bool valid(const std::tuple<int,int>& args){return std::get<0>(args)>0&&std::get<0>(args)<=4096&&std::get<1>(args)>0&&std::get<1>(args)<=4096;}
    static std::size_t bytes(const std::tuple<int,int>& args){return std::size_t(std::get<0>(args))*std::get<1>(args)*4;}
    static void launch(resources& r,const std::tuple<int,int>& args){typed_cuda::launch_render(r,std::get<0>(args),std::get<1>(args));}
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
auto bind_gpu(std::shared_ptr<gpu_backend::context> context) {
    using D=dynabridge::gpu<gpu_backend::context>;
#ifdef FF_TYPED_ERASED_DEMO
    return dynabridge::bind_async<dynabridge::gpu>(std::move(context),D::object_t{});
#else
    return dynabridge::bind_static_async<dynabridge::gpu>(std::move(context),D::object_t{});
#endif
}
bool image_valid(const image_result& image) noexcept {
    if(image.width!=128 || image.height!=96 || image.pixels.size()!=128*96*4) return false;
    bool colored=false;
    for(std::size_t i=0;i<image.pixels.size();i+=4) {
        if(image.pixels[i+3]!=255) return false;
        colored=colored || image.pixels[i] || image.pixels[i+1] || image.pixels[i+2];
    }
    return colored;
}
bool release_owner_from_continuation() {
    auto owner=std::make_shared<gpu_backend::context>();
    std::weak_ptr<gpu_backend::context> lifetime=owner;
    bool completed=false,valid=false;
    {
        auto gpu=bind_gpu(owner);
        dynabridge::ff::start_invocation(gpu.add_one(41),[&](flux_foundry::result_t<int,dynabridge::async_error> result) noexcept {
            valid=result.has_value() && result.value()==42;
            completed=true;owner->close();owner.reset();
        });
    }
    while(!completed) {owner->poll();std::this_thread::sleep_for(std::chrono::microseconds(100));}
    return valid && lifetime.expired();
}
}
int main(int argc,char**){
    int devices=0;if(cudaGetDeviceCount(&devices)!=cudaSuccess || !devices){std::puts("SKIP: no CUDA device");return 77;}
    auto ctx=std::make_shared<gpu_backend::context>();ctx->fail_record=argc>1;
    auto gpu=bind_gpu(ctx);
    int completed=0;bool valid=true;
    dynabridge::ff::start_invocation(gpu.add_one(41),[&](flux_foundry::result_t<int,dynabridge::async_error> r) noexcept {valid&=ctx->fail_record?!r.has_value():(r.has_value()&&r.value()==42);++completed;});
    dynabridge::ff::start_invocation(gpu.render(128,96),[&](flux_foundry::result_t<image_result,dynabridge::async_error> r) noexcept {valid&=ctx->fail_record?!r.has_value():(r.has_value()&&image_valid(r.value()));++completed;ctx->close();});
    while(!ctx->pending.empty()){ctx->poll();std::this_thread::sleep_for(std::chrono::microseconds(100));}
    valid=valid && completed==2 && release_owner_from_continuation();
    std::printf("[%s] typed CUDA scalar, image, record-failure=%d, continuation close/release\n",valid?"OK":"FAIL",int(ctx->fail_record));
    return valid?0:1;
}
