// Pooled-context experiment, 2026-11-18.
//
// benchmark.cpp gives every operation its own stream, event, cudaMalloc and pageable
// host buffer. That per-operation lifecycle, not the framework, dominates the cost:
// on an RTX 2080 Ti the same kernel reaches roughly 6-7x the throughput once the CUDA
// resources belong to the context and are rotated across the window slots. The
// blueprint stays static; only the context changes from one invocation to the next.
//
//   native        : per-operation stream/event/cudaMalloc/pageable host (benchmark.cpp)
//   native_pool   : identical completion path, resources owned by the context
//   concrete_pool : FF static typed invocation over the pooled contexts
//   erased_pool   : FF erased typed invocation over the pooled contexts
//
// One context per in-flight slot, each owning its stream, disable-timing event,
// device buffer and pinned host buffer. submit_render() branches on the context: a
// pooled context reuses its resources, a plain context keeps the per-operation
// behaviour so the control stays in the same binary and the same process.
//
// Measured 2026-11-18, RTX 2080 Ti (driver 620.70.06, toolkit 13.4.3, sm_75),
// Release, five samples per mode, image order rotated every sample. Medians in
// completed images/s, parenthesised column is the paired deviation from native_pool:
//
//   image   window   native   native_pool   concrete_pool      erased_pool
//   128x128      1   28,689        41,445          40,936 (-1.2%)   41,389 (-0.1%)
//   128x128      8   28,502       114,743         112,529 (-1.9%)  110,399 (-3.8%)
//   512x512      1    4,517         6,121           6,108 (-0.2%)    6,071 (-0.8%)
//   512x512      8    1,642        12,638          12,711 (+0.6%)   12,693 (+0.4%)
//
// Reading these numbers:
//  - FF + Portal cost 0-4% against the pooled native control; the gain is not theirs.
//  - The `native` row here is faster than in benchmark.cpp (28,689 vs ~6,700 at
//    128x128 window 1) because this process keeps pinned and device allocations
//    alive for the pooled slots, which keeps the driver allocator warm. In a
//    process that never pools, per-operation allocation reproduces the
//    benchmark.cpp figures. Both baselines are reported; the pooled comparison is
//    the paired one.
//  - 512x512 window 8 plateaus near 12,700 images/s in every mode: 1 MiB per image
//    over PCIe 3.0 x16, so the remaining cost is the device-to-host transfer.
//  - The result type still copies each image out of the pinned buffer into a fresh
//    std::vector. That materialisation is measured separately in bench/cuda/README.md
//    and costs another 5-27% depending on image size; it belongs to the demo result
//    type, not to FF.
#include <portal/extensions/flux_foundry/async.h>
#include <portal/backend_base.h>
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
#define PORTAL_IMPORT_DEF "cuda_typed.def"
#include <portal/import.h>
#undef PORTAL_IMPORT_DEF
namespace ff = flux_foundry;
namespace db = portal;

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
// A pooled context owns its CUDA resources; the blueprint never sees them.
struct context : std::enable_shared_from_this<context> {
    using backend_t=gpu_backend;
    std::list<std::function<bool()>> pending;
    resources pooled;
    unsigned char* pinned=nullptr;
    std::size_t bytes=0;
    bool is_pooled=false;
    void init_pooled(int width,int height) {
        bytes=std::size_t(width)*std::size_t(height)*4u;
        cu(cudaStreamCreateWithFlags(&pooled.stream,cudaStreamNonBlocking),"pool stream");
        cu(cudaEventCreateWithFlags(&pooled.event,cudaEventDisableTiming),"pool event");
        cu(cudaMalloc(&pooled.device,bytes),"pool device alloc");
        cu(cudaHostAlloc(reinterpret_cast<void**>(&pinned),bytes,cudaHostAllocDefault),"pool pinned alloc");
        is_pooled=true;
    }
    ~context() { if(pinned) cudaFreeHost(pinned); }
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
    if(ctx.is_pooled) {
        ctx.pending.emplace_back([&ctx,width,height,callback=std::move(callback)]() mutable {
            if(cudaEventQuery(ctx.pooled.event)!=cudaSuccess) return false;
            image_result result{width,height,std::vector<unsigned char>(ctx.pinned,ctx.pinned+ctx.bytes)};
            callback(std::move(result));
            return true;
        });
        typed_cuda::launch_render(ctx.pooled,width,height);
        cu(cudaGetLastError(),"kernel launch");
        cu(cudaMemcpyAsync(ctx.pinned,ctx.pooled.device,ctx.bytes,cudaMemcpyDeviceToHost,ctx.pooled.stream),"D2H");
        cu(cudaEventRecord(ctx.pooled.event,ctx.pooled.stream),"record");
        return;
    }
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

// One FF launch per pooled context: static blueprint, per-slot view, fresh runner per operation.
template<class View>
void add_ff_launch(std::vector<std::function<void(std::size_t*)>>& out,std::shared_ptr<View> view,verification& check) {
    using call_t=decltype(view->render(1,1));
    auto bp=ff::make_blueprint<call_t,db::async_error>() | db::ff::await_static_invocation<call_t>()
        | ff::end([](output&& r) noexcept -> output {return std::move(r);});
    auto shared=ff::make_lite_ptr<decltype(bp)>(std::move(bp));
    ck(bool(shared),"blueprint allocation");
    out.push_back([view,shared,&check](std::size_t* done) {
        auto runner=ff::make_fast_runner(shared,sink{&check,done});
        runner(view->render(check.width,check.height));
    });
}

// Polls every context it is given; the caller rotates the launch across window slots.
template<class Launch>
double trial(std::vector<std::shared_ptr<context>>& ctxs,verification& check,int n,int window,Launch launch) {
    std::size_t issued=0,done=0;
    const auto begin=std::chrono::steady_clock::now();
    while(done<std::size_t(n)) {
        while(issued<std::size_t(n) && issued-done<std::size_t(window)) { launch(&done); ++issued; }
        bool polled=false;
        for(auto& c : ctxs) if(!c->pending.empty()) { c->poll(); polled=true; }
        if(!polled) ck(done==issued,"lost operation");
    }
    auto end=std::chrono::steady_clock::now();
    ck(issued==done,"drain");
    for(auto& c : ctxs) ck(c->pending.empty(),"empty pending");
    return std::chrono::duration<double>(end-begin).count();
}

const char* mode_name(int mode) {
    switch(mode) {
        case 0: return "native";
        case 1: return "native_pool";
        case 2: return "concrete_pool";
        default: return "erased_pool";
    }
}

int main() {
    cu(cudaFree(nullptr),"init");
    int device=0; cudaDeviceProp prop{}; cu(cudaGetDevice(&device),"device"); cu(cudaGetDeviceProperties(&prop,device),"props");
    std::fprintf(stderr,"device: %s sm_%d%d\n",prop.name,prop.major,prop.minor);
    std::printf("kind,mode,width,height,window,rep,seconds,fps\n");
    for(int size : {128,512}) {
        verification check; check.width=size; check.height=size;
        const int n=size==128?6000:1500;
        for(int window : {1,8}) {
            std::vector<std::shared_ptr<context>> slots;
            for(int i=0;i<window;++i) { auto c=std::make_shared<context>(); c->init_pooled(size,size); slots.push_back(c); }
            auto legacy=std::make_shared<context>();
            using D=db::gpu<context>;
            std::vector<std::function<void(std::size_t*)>> concrete,erased;
            for(auto& c : slots) {
                auto cv=db::bind_static_async<db::gpu>(c,D::object_t{});
                add_ff_launch(concrete,std::make_shared<std::decay_t<decltype(cv)>>(cv),check);
                auto ev=db::bind_async<db::gpu>(c,D::object_t{});
                add_ff_launch(erased,std::make_shared<std::decay_t<decltype(ev)>>(ev),check);
            }
            std::vector<std::shared_ptr<context>> native_ctxs{legacy};
            std::size_t rr=0;
            std::function<void(std::size_t*)> modes[4];
            modes[0]=[&](std::size_t* done) {
                submit_render(*legacy,size,size,[&check,done](image_result r){ check.accept(r); ++*done; });
            };
            modes[1]=[&](std::size_t* done) {
                auto& c=slots[rr % slots.size()]; ++rr;
                submit_render(*c,size,size,[&check,done](image_result r){ check.accept(r); ++*done; });
            };
            modes[2]=[&](std::size_t* done) { auto& f=concrete[rr % concrete.size()]; ++rr; f(done); };
            modes[3]=[&](std::size_t* done) { auto& f=erased[rr % erased.size()]; ++rr; f(done); };
            std::vector<std::shared_ptr<context>>* mode_ctx[4]{&native_ctxs,&slots,&slots,&slots};
            for(int mode=0;mode<4;++mode) {
                trial(*mode_ctx[mode],check,8,window,modes[mode]); // warm
            }
            for(int rep=0;rep<5;++rep) {
                for(int k=0;k<4;++k) {
                    const int mode=(k+rep)%4;
                    double sec=trial(*mode_ctx[mode],check,n,window,modes[mode]);
                    double fps=n/sec;
                    std::printf("RUN,%s,%d,%d,%d,%d,%.9f,%.3f\n",mode_name(mode),size,size,window,rep,sec,fps);
                    std::fflush(stdout);
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        }
        std::printf("VERIFY,%d,%d,%llu\n",size,size,(unsigned long long)check.count);
        std::fflush(stdout);
    }
    return 0;
}
