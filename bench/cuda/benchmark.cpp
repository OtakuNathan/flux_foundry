// Temporary matched GPU throughput experiment, 2026-11-05.
// Same per-operation CUDA resources, completion queue and validation in all end-to-end modes.
#include <dynabridge/extensions/flux_foundry/async.h>
#include <dynabridge/backend_base.h>
#include "cuda_typed_device.h"
#include "cuda_completion_condition.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <list>
#include <memory>
#include <thread>
#include <tuple>
#include <vector>
#define DYNABRIDGE_IMPORT_DEF "cuda_typed.def"
#include <dynabridge/import.h>
#undef DYNABRIDGE_IMPORT_DEF
namespace ff = flux_foundry;
namespace db = dynabridge;

static void ck(bool value, const char* what) { if (!value) { std::fprintf(stderr,"FAILED: %s\n",what); std::abort(); } }
static void cu(cudaError_t code, const char* what) { if (code != cudaSuccess) { std::fprintf(stderr,"CUDA FAILED: %s: %s\n",what,cudaGetErrorString(code)); std::abort(); } }
using resources=typed_cuda::resources;
struct context;

template<class Callback>
void submit_render(context& ctx,int width,int height,Callback callback);
struct gpu_backend:db::backend_base<gpu_backend> {
    using context=::context;
    template<class R,class Direction> struct object_t {};
    template<class Symbol,class Contract,class Receiver,class Tuple>
    static auto prepare_async(std::shared_ptr<Receiver> receiver,Tuple args) {
        static_assert(std::is_same<Symbol,db::import_symbols::gpu::render>::value,"render only");
        return db::make_invocation<image_result>(std::move(receiver),[args=std::move(args)](Receiver& receiver,auto done) {
            submit_render(receiver.context(),std::get<0>(args),std::get<1>(args),[done](image_result result) noexcept {
                done.finish(db::async_result<image_result>(std::move(result)));
            });
            return db::async_submission::accept();
        });
    }
};
struct context : std::enable_shared_from_this<context> {
    using backend_t=gpu_backend;
    std::list<std::function<bool()>> pending;
    void poll() {
        auto keep=shared_from_this();
        std::list<std::function<bool()>> round;
        round.splice(round.end(),pending);
        while(!round.empty()) {
            auto current=round.begin();
            if((*current)()) round.erase(current); else pending.splice(pending.end(),round,current);
        }
    }
};
template<class Callback>
void submit_render(context& ctx,int width,int height,Callback callback) {
    auto state=std::make_shared<resources>();
    state->host.resize(std::size_t(width)*height*4);
    ctx.pending.emplace_back([state,width,height,callback=std::move(callback)]() mutable {
        int error=int(state->error);
        if(!cuda_completion_ready(state->recorded,error,int(cudaSuccess),int(cudaErrorNotReady),
             [&]{return int(cudaEventQuery(state->event));},[&]{return int(cudaStreamSynchronize(state->stream));})) return false;
        cu(static_cast<cudaError_t>(error),"completion");
        image_result result{width,height,std::move(state->host)};
        state.reset();
        callback(std::move(result));
        return true;
    });
    cu(cudaStreamCreateWithFlags(&state->stream,cudaStreamNonBlocking),"stream create");
    cu(cudaEventCreateWithFlags(&state->event,cudaEventDisableTiming),"event create");
    cu(cudaMalloc(&state->device,state->host.size()),"device alloc");
    typed_cuda::launch_render(*state,width,height);
    cu(cudaGetLastError(),"kernel launch");
    cu(cudaMemcpyAsync(state->host.data(),state->device,state->host.size(),cudaMemcpyDeviceToHost,state->stream),"D2H");
    cu(cudaEventRecord(state->event,state->stream),"record");
    state->recorded=true;
}

struct verification {
    int width,height;
    std::vector<unsigned char> golden;
    std::uint64_t checksum=0, count=0;
    void accept(const image_result& r) {
        ck(r.width==width && r.height==height && r.pixels.size()==std::size_t(width)*height*4,"image shape");
        std::vector<unsigned char> sample; sample.reserve(257*4);
        for(std::size_t i=0;i<257;++i) {
            const std::size_t ix=((i*7919u+17u)%(std::size_t(width)*height))*4;
            ck(r.pixels[ix+3]==255,"alpha");
            for(int j=0;j<4;++j) sample.push_back(r.pixels[ix+j]);
        }
        if(golden.empty()) {
            golden=sample;
            bool colored=false; for(std::size_t i=0;i<sample.size();i+=4) colored=colored || sample[i] || sample[i+1] || sample[i+2];
            ck(colored,"nonblack sampled pixels");
        } else ck(sample==golden,"pixel sample mismatch");
        checksum += sample[0]+sample[4]+sample[8]+sample[12]+1;
        ++count;
    }
};
using output=ff::result_t<image_result,db::async_error>;
struct sink {
    using value_type=output;
    verification* check; std::size_t* complete;
    void emplace(output&& r) noexcept {
        ck(r.has_value(),"FF completion"); check->accept(r.value()); ++*complete;
    }
};
template<class Launch>
double trial(context& ctx,verification& check,int n,int window,Launch launch) {
    std::size_t issued=0,done=0;
    const auto begin=std::chrono::steady_clock::now();
    while(done<std::size_t(n)) {
        while(issued<std::size_t(n) && issued-done<std::size_t(window)) {
            launch(&done); ++issued;
        }
        if(!ctx.pending.empty()) ctx.poll();
        else ck(done==issued,"lost operation");
    }
    auto end=std::chrono::steady_clock::now();
    ck(issued==done && ctx.pending.empty(),"drain");
    return std::chrono::duration<double>(end-begin).count();
}

struct native_path {
    std::shared_ptr<context> ctx; verification* check;
    void launch(std::size_t* done) {
        submit_render(*ctx,check->width,check->height,[this,done](image_result r) {check->accept(r);++*done;});
    }
};
// Factory avoids lambda-in-unevaluated-context, which is not available in C++14.
template<class View,class Callback>
void with_typed_path(std::shared_ptr<context> ctx,verification& check,View gpu,Callback callback) {
    using call_t=decltype(gpu.render(1,1));
    auto bp=ff::make_blueprint<call_t,db::async_error>() | db::ff::await_static_invocation<call_t>()
        | ff::end([](output&& r) noexcept -> output {return std::move(r);});
    auto shared=ff::make_lite_ptr<decltype(bp)>(std::move(bp)); ck(bool(shared),"blueprint allocation");
    auto launch=[&](std::size_t* done) {
        auto runner=ff::make_fast_runner(shared,sink{&check,done});
        runner(gpu.render(check.width,check.height));
    };
    callback(launch);
}

// Device-only measurement intentionally excludes allocations, D2H and completion.
void gpu_kernel_only(verification& check,int count) {
    resources state;
    state.host.resize(std::size_t(check.width)*check.height*4);
    cu(cudaStreamCreateWithFlags(&state.stream,cudaStreamNonBlocking),"gpuonly stream");
    cu(cudaMalloc(&state.device,state.host.size()),"gpuonly alloc");
    for(int i=0;i<5;++i) typed_cuda::launch_render(state,check.width,check.height);
    cu(cudaGetLastError(),"gpuonly warmup launch"); cu(cudaStreamSynchronize(state.stream),"gpuonly warmup sync");
    cudaEvent_t start{},end{}; cu(cudaEventCreate(&start),"gpuonly event start");cu(cudaEventCreate(&end),"gpuonly event end");
    cu(cudaEventRecord(start,state.stream),"gpuonly start");
    for(int i=0;i<count;++i) typed_cuda::launch_render(state,check.width,check.height);
    cu(cudaGetLastError(),"gpuonly launches");cu(cudaEventRecord(end,state.stream),"gpuonly end");cu(cudaEventSynchronize(end),"gpuonly sync");
    float ms=0;cu(cudaEventElapsedTime(&ms,start,end),"gpuonly time");
    cu(cudaMemcpy(state.host.data(),state.device,state.host.size(),cudaMemcpyDeviceToHost),"gpuonly verify copy");
    check.accept(image_result{check.width,check.height,std::move(state.host)});
    std::printf("GPU_ONLY,%d,%d,%d,%.6f,%.3f\n",check.width,check.height,count,ms,1000.0*count/ms);
    cu(cudaEventDestroy(start),"destroy start");cu(cudaEventDestroy(end),"destroy end");
}

int main() {
    cu(cudaFree(nullptr),"init");
    std::printf("kind,width,height,window_or_count,repeat_or_ms,seconds_or_fps,fps,pixels_Mps\n");
    for(int size : {128,512}) {
        verification check{size,size};
        gpu_kernel_only(check,size==128?500:60);
        const int n=size==128?6000:1500;
        for(int window : {1,8}) {
            auto c=std::make_shared<context>();
            using D=db::gpu<context>;
            auto concrete=db::bind_static_async<db::gpu>(c,D::object_t{});
            auto erased=db::bind_async<db::gpu>(c,D::object_t{});
            native_path native{c,&check};
            with_typed_path(c,check,concrete,[&](auto& c_launch) {
                with_typed_path(c,check,erased,[&](auto& e_launch) {
                    auto n_launch=[&](std::size_t* done){native.launch(done);};
                    // Warm all paths on the same context; rotate order on every measured repeat.
                    trial(*c,check,8,window,n_launch);trial(*c,check,8,window,c_launch);trial(*c,check,8,window,e_launch);
                    for(int rep=0;rep<5;++rep) {
                        for(int k=0;k<3;++k) {
                            int mode=(k+rep)%3; double sec=0;const char* name="";
                            if(mode==0){name="native";sec=trial(*c,check,n,window,n_launch);}
                            if(mode==1){name="concrete";sec=trial(*c,check,n,window,c_launch);}
                            if(mode==2){name="erased";sec=trial(*c,check,n,window,e_launch);}
                            double fps=n/sec;
                            std::printf("RUN,%s,%d,%d,%d,%d,%.9f,%.3f,%.3f\n",name,size,size,window,rep,sec,fps,fps*size*size/1e6);
                            std::fflush(stdout);
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        }
                    }
                });
            });
        }
        std::printf("VERIFY,%d,%d,%llu,%llu\n",size,size,(unsigned long long)check.count,(unsigned long long)check.checksum);
    }
}
