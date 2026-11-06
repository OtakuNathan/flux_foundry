// Real event-loop backends for the same scheduled-arrival synthetic workload.
#include "bench/load_probe.h"
#include "executor/gsource_executor.h"
#include <uv.h>
#include <ctime>
#include <string>

namespace event_probe {
using namespace load_probe;
using namespace flux_foundry;

template<class T> class AlignedOwner {
    T* ptr_;
public:
    template<class... Args> explicit AlignedOwner(Args&&... args) : ptr_(nullptr) {
        void* p = flux_foundry::aligned_alloc(alignof(T),sizeof(T));
        require(p != nullptr,"aligned backend allocation failed");
        try { ptr_ = new (p) T(std::forward<Args>(args)...); }
        catch (...) { flux_foundry::aligned_free(p); throw; }
    }
    ~AlignedOwner() { ptr_->~T(); flux_foundry::aligned_free(ptr_); }
    T* get() const { return ptr_; }
    AlignedOwner(const AlignedOwner&) = delete;
    AlignedOwner& operator=(const AlignedOwner&) = delete;
};

// Benchmark adapter, not a new production executor API. One owner drains the
// ff MPSC queue; uv_async coalesces notifications, and budget exhaustion rearms.
template<std::size_t Capacity> class UvExecutor {
    mpsc_queue<task_wrapper_sbo,Capacity> queue_;
    uv_loop_t loop_{};
    uv_async_t async_{};
    const std::size_t budget_;
    std::thread::id owner_;
    static void callback(uv_async_t* handle) noexcept {
        auto* self = static_cast<UvExecutor*>(handle->data);
        std::size_t count = 0;
        for (; count < self->budget_; ++count) {
            auto task = self->queue_.try_pop();
            if (!task) break;
            task.get()();
        }
        // A stop task may have closed this handle during this callback.
        if (count == self->budget_ && !uv_is_closing(reinterpret_cast<uv_handle_t*>(handle)))
            require(uv_async_send(handle) == 0,"libuv rearm failed");
    }
public:
    explicit UvExecutor(std::size_t budget) : budget_(budget) {}
    void run(std::atomic<bool>& ready) {
        owner_ = std::this_thread::get_id();
        require(uv_loop_init(&loop_) == 0,"uv_loop_init failed");
        require(uv_async_init(&loop_,&async_,callback) == 0,"uv_async_init failed");
        async_.data = this;
        ready.store(true,std::memory_order_release);
        uv_run(&loop_,UV_RUN_DEFAULT);
        require(uv_loop_close(&loop_) == 0,"libuv handles were not closed");
    }
    void dispatch(task_wrapper_sbo&& task) noexcept {
        for (backoff_strategy<> backoff; !queue_.try_emplace(std::move(task)); backoff.yield()) {
            if (std::this_thread::get_id() == owner_) { task(); return; }
        }
        require(uv_async_send(&async_) == 0,"uv_async_send failed");
    }
    void stop() noexcept { uv_close(reinterpret_cast<uv_handle_t*>(&async_),nullptr); }
};

struct Config {
    Options load;
    std::string backend = "all";
    std::size_t batch = 64;
    bool explicit_batch = false;
};
static Config parse_config(int argc,char** argv) {
    Config c;
    bool idle = false;
    std::vector<char*> shared;
    shared.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i],"--help")) {
            std::puts("Event-loop load probe (exceptions enabled for gsource constructor).\n"
                      "--backend all|simple|gsource|libuv --batch N (libuv only; default compares 10 and 64)\n"
                      "--capacity 256|1024|4096|16384 (default 4096)\n"
                      "--samples N --repeats N --work N --mode all|raw|fast|normal\n"
                      "--profile all|idle1k|steady100k|steady500k|overload2m|burst8192|stall5ms\n"
                      "--producer-cpu N --consumer-cpu N\n"
                      "idle1k is capped at 1024 events/run; omit CPU options for unpinned runs.");
            std::exit(0);
        }
        require(i+1 < argc,"missing option value");
        char* key = argv[i++]; char* val = argv[i];
        if (!std::strcmp(key,"--backend")) c.backend = val;
        else if (!std::strcmp(key,"--batch")) {
            c.batch = number(val); c.explicit_batch = true;
            require(c.batch > 0 && c.batch <= 16384,"batch must be 1..16384");
        } else {
            shared.push_back(key);
            if (!std::strcmp(key,"--profile") && !std::strcmp(val,"idle1k")) {
                idle = true;
                shared.push_back(const_cast<char*>("all"));
            } else shared.push_back(val);
        }
    }
    c.load = parse(static_cast<int>(shared.size()),shared.data());
    if (idle) c.load.profile = "idle1k";
    if (!c.load.capacity) c.load.capacity = 4096;
    require(c.backend=="all" || c.backend=="simple" || c.backend=="gsource" || c.backend=="libuv","invalid backend");
    return c;
}
struct ThreadCpu {
    clockid_t clock;
    explicit ThreadCpu(std::thread& thread) {
        require(pthread_getcpuclockid(thread.native_handle(),&clock) == 0,"cannot get consumer CPU clock");
    }
    static Ns snapshot(void* ptr) {
        auto* self = static_cast<ThreadCpu*>(ptr);
        timespec ts{};
        require(clock_gettime(self->clock,&ts) == 0,"cannot read consumer CPU clock");
        return static_cast<Ns>(ts.tv_sec)*INT64_C(1000000000)+ts.tv_nsec;
    }
};
static void matrix(Port port,std::size_t capacity,const Config& c,const char* backend,std::size_t budget,std::thread& consumer) {
    ThreadCpu clock(consumer);
    CpuMonitor monitor{&clock,ThreadCpu::snapshot};
    const Profile idle{"idle1k",1000,0,0,0};
    std::vector<Profile> cases{idle};
    for (const auto& p : profiles) cases.push_back(p);
    const char* modes[] = {"raw","fast","normal"};
    const std::string prefix = std::string(backend)+","+std::to_string(budget)+",";
    for (unsigned r=1; r<=c.load.repeats; ++r) {
        for (const auto& p : cases) {
            if (std::strcmp(c.load.profile,"all") && std::strcmp(c.load.profile,p.name)) continue;
            Options o = c.load;
            if (!std::strcmp(p.name,"idle1k")) o.samples = std::min<std::size_t>(o.samples,1024);
            for (unsigned m=0; m<3; ++m) {
                const char* mode=modes[(m+r-1)%3];
                if (std::strcmp(o.mode,"all") && std::strcmp(o.mode,mode)) continue;
                const auto time=measured_run(port,capacity,o,p,mode,r,prefix.c_str(),false,&monitor);
                require(time.wall_ns>0 && time.consumer_cpu_ns>=0,"invalid CPU measurement");
                std::printf("%.3f,%.3f\n",time.consumer_cpu_ns/1e6,100.0*time.consumer_cpu_ns/time.wall_ns);
                std::fflush(stdout);
            }
        }
    }
}
template<std::size_t Capacity> static void simple(const Config& c) {
    AlignedOwner<simple_executor<Capacity>> holder;
    auto* ex=holder.get();
    Port port{ex,[](void* p,task_wrapper_sbo&& task) noexcept { static_cast<simple_executor<Capacity>*>(p)->dispatch(std::move(task)); }};
    std::atomic<bool> ready{false};
    std::thread consumer([&]() { pin(c.load.consumer_cpu); ready.store(true,std::memory_order_release); ex->run(); });
    while(!ready.load(std::memory_order_acquire)) relax();
    matrix(port,Capacity,c,"simple",0,consumer);
    require(ex->try_shutdown(),"simple shutdown failed");
    consumer.join();
}
template<std::size_t Capacity> static void gsource(const Config& c) {
    auto* context=g_main_context_new();
    auto* loop=g_main_loop_new(context,FALSE);
    {
        AlignedOwner<gsource_executor<Capacity>> holder;
        auto* ex=holder.get();
        require(ex->register_to(context)==0,"gsource registration failed");
        Port port{ex,[](void* p,task_wrapper_sbo&& task) noexcept { static_cast<gsource_executor<Capacity>*>(p)->dispatch(std::move(task)); }};
        std::atomic<bool> ready{false};
        std::thread consumer([&]() {
            pin(c.load.consumer_cpu);
            g_main_context_push_thread_default(context);
            ready.store(true,std::memory_order_release);
            g_main_loop_run(loop);
            g_main_context_pop_thread_default(context);
        });
        while(!ready.load(std::memory_order_acquire)) relax();
        matrix(port,Capacity,c,"gsource",gsource_executor<Capacity>::max_task_per_round,consumer);
        ex->dispatch(task_wrapper_sbo([loop]() noexcept { g_main_loop_quit(loop); }));
        consumer.join();
    }
    g_main_loop_unref(loop);
    g_main_context_unref(context);
}
template<std::size_t Capacity> static void libuv(const Config& c,std::size_t budget) {
    AlignedOwner<UvExecutor<Capacity>> holder(budget);
    auto* ex=holder.get();
    Port port{ex,[](void* p,task_wrapper_sbo&& task) noexcept { static_cast<UvExecutor<Capacity>*>(p)->dispatch(std::move(task)); }};
    std::atomic<bool> ready{false};
    std::thread consumer([&]() { pin(c.load.consumer_cpu); ex->run(ready); });
    while(!ready.load(std::memory_order_acquire)) relax();
    matrix(port,Capacity,c,"libuv",budget,consumer);
    // Do not close the async handle until the producer's final send returned.
    std::atomic<bool> stop_sent{false};
    ex->dispatch(task_wrapper_sbo([ex,&stop_sent]() noexcept {
        while (!stop_sent.load(std::memory_order_acquire)) relax();
        ex->stop();
    }));
    stop_sent.store(true,std::memory_order_release);
    consumer.join();
}
template<std::size_t Capacity> static void run(const Config& c) {
    if (c.backend=="all" || c.backend=="simple") simple<Capacity>(c);
    if (c.backend=="all" || c.backend=="gsource") gsource<Capacity>(c);
    if (c.backend=="all" || c.backend=="libuv") {
        if (!c.explicit_batch && c.batch!=10) libuv<Capacity>(c,10);
        libuv<Capacity>(c,c.batch);
    }
}
template<std::size_t Capacity> static void uv_contract(std::size_t budget) {
    AlignedOwner<UvExecutor<Capacity>> holder(budget);
    auto* ex=holder.get();
    std::atomic<bool> ready{false};
    std::atomic<std::size_t> delivered{0};
    std::thread consumer([&]() { ex->run(ready); });
    while(!ready.load(std::memory_order_acquire)) relax();
    ex->dispatch(task_wrapper_sbo([&]() noexcept {
        for (std::size_t i=0; i<Capacity+7; ++i)
            ex->dispatch(task_wrapper_sbo([&]() noexcept { delivered.fetch_add(1,std::memory_order_relaxed); }));
    }));
    for (std::size_t i=0; i<1000; ++i)
        ex->dispatch(task_wrapper_sbo([&]() noexcept { delivered.fetch_add(1,std::memory_order_relaxed); }));
    std::atomic<bool> fenced{false};
    ex->dispatch(task_wrapper_sbo([&]() noexcept { fenced.store(true,std::memory_order_release); }));
    const auto timeout=now_ns()+INT64_C(5000000000);
    while(!fenced.load(std::memory_order_acquire)) { require(now_ns()<timeout,"uv contract timeout"); relax(); }
    require(delivered.load(std::memory_order_acquire)==Capacity+1007,"uv wakeup/inline contract lost tasks");
    std::atomic<bool> stop_sent{false};
    ex->dispatch(task_wrapper_sbo([&]() noexcept {
        while(!stop_sent.load(std::memory_order_acquire)) relax();
        ex->stop();
    }));
    stop_sent.store(true,std::memory_order_release);
    consumer.join();
}
static void self_test() {
    for (const auto budget : {std::size_t(1),std::size_t(10),std::size_t(64)}) {
        uv_contract<1>(budget); uv_contract<2>(budget);
    }
    std::puts("PASS: libuv nested full-queue inline, external producer wakeup/coalescing, budget rearm and shutdown (six configurations)");
}
}
int main(int argc,char** argv) {
    using namespace event_probe;
    if (argc==2 && !std::strcmp(argv[1],"--self-test")) { self_test(); return 0; }
    const Config c=parse_config(argc,argv);
    pin(c.load.producer_cpu);
    std::fprintf(stderr,"real-loop probe: GLib=%u.%u.%u libuv=%s; exceptions enabled, one producer/one loop owner, synthetic work, no network IO\n",glib_major_version,glib_minor_version,glib_micro_version,uv_version_string());
    print_csv_header("backend,budget,",",consumer_cpu_ms,consumer_cpu_pct");
    switch(c.load.capacity) {
        case 256: run<256>(c); break;
        case 1024: run<1024>(c); break;
        case 4096: run<4096>(c); break;
        case 16384: run<16384>(c); break;
        default: require(false,"unsupported capacity");
    }
}
