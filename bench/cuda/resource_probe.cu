// CUDA-side resource probe, 2026-11-18.
//
// Isolates the two levers that sit below the framework, using the same Mandelbrot
// kernel, the same windowing and the same 257-sample verification as benchmark.cpp:
//
//   native      : per-operation stream + event + cudaMalloc + pageable host
//   pool        : per-slot resources, pinned host buffer, reused device buffer
//   graph       : pool + a captured kernel+D2H graph, replayed per operation
//   graph_param : graph + cudaGraphExecKernelNodeSetParams on every operation
//
// Two environment switches:
//   PROBE_ONLY_NATIVE=1  run only the per-operation path, so the process never
//                        pools anything and the allocator stays as cold as it is
//                        for benchmark.cpp.
//   PROBE_COPY=1         materialise each image into a fresh std::vector from the
//                        pinned buffer, emulating a by-value result type.
//
// Measured 2026-11-18, RTX 2080 Ti, medians of two independent runs (five samples
// each), completed images/s:
//
//   image   window   native   pool     graph
//   128x128      1    6,124   39,645   41,483
//   128x128      8   17,036  122,397  128,994
//   512x512      1    2,752    6,855    7,298
//   512x512      8    1,386   12,020   12,138
//
// Findings: pooling dominates (6-9x at window 8); graph replay adds about 5-9% at
// 128x128 and nothing at 512x512, where the D2H transfer over PCIe 3.0 x16 caps
// every mode near 12,700 images/s. Replaying a graph with per-operation parameter
// updates costs about 0.5 us per operation. Run-to-run spread at 128x128 window 8
// is roughly +/-10%, so treat single-digit differences as noise.
//
// PROBE_COPY=1 lowers those pooled figures by 5-27% depending on image size: that
// is the demo result type's materialisation cost, and the pooled native path pays
// it too.
#include <cuda_runtime.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "cuda_image_kernel.cuh"

namespace {
void ck(bool v, const char* what) {
    if (!v) { std::fprintf(stderr, "FAILED: %s\n", what); std::abort(); }
}
void cu(cudaError_t c, const char* what) {
    if (c != cudaSuccess) {
        std::fprintf(stderr, "CUDA FAILED %s: %s\n", what, cudaGetErrorString(c));
        std::abort();
    }
}
constexpr int kSamples = 257;

struct checker {
    int w = 0, h = 0;
    std::vector<unsigned char> golden;
    std::size_t count = 0;
    void accept(const unsigned char* px) {
        std::vector<unsigned char> s;
        s.reserve(kSamples * 4);
        for (int i = 0; i < kSamples; ++i) {
            const std::size_t ix = ((std::size_t(i) * 7919u + 17u) % (std::size_t(w) * h)) * 4;
            ck(px[ix + 3] == 255, "alpha");
            for (int j = 0; j < 4; ++j) s.push_back(px[ix + j]);
        }
        if (golden.empty()) {
            golden = s;
            bool colored = false;
            for (std::size_t i = 0; i < s.size(); i += 4) colored = colored || s[i] || s[i + 1] || s[i + 2];
            ck(colored, "nonblack sampled pixels");
        } else {
            ck(s == golden, "pixel sample mismatch");
        }
        ++count;
    }
};

struct slot {
    cudaStream_t stream = nullptr;
    cudaEvent_t ev = nullptr;
    void* dev = nullptr;
    unsigned char* host = nullptr;
    cudaGraph_t graph = nullptr;
    cudaGraphExec_t exec = nullptr;
    cudaGraphNode_t knode = nullptr;
    cudaKernelNodeParams kp{};
    int kw = 0, kh = 0;
    float kscale = 1.f;
    void* kargs[4] = {nullptr, nullptr, nullptr, nullptr};
};

struct op_state {
    cudaStream_t stream = nullptr;
    cudaEvent_t ev = nullptr;
    void* dev = nullptr;
    std::vector<unsigned char> host;
};

unsigned long long bytes_of(int w, int h) { return (unsigned long long)w * h * 4ull; }

// Optional: emulate the demo's by-value result type (fresh std::vector per operation).
static bool g_copy_result = false;
static volatile unsigned long long g_sink = 0;

void build_pooled(std::vector<slot>& slots, int w, int h, int window, bool with_graph) {
    slots.clear();
    slots.resize((std::size_t)window);
    const unsigned long long bytes = bytes_of(w, h);
    for (auto& s : slots) {
        cu(cudaStreamCreateWithFlags(&s.stream, cudaStreamNonBlocking), "stream create");
        cu(cudaEventCreateWithFlags(&s.ev, cudaEventDisableTiming), "event create");
        cu(cudaMalloc(&s.dev, (std::size_t)bytes), "device alloc");
        cu(cudaHostAlloc((void**)&s.host, (std::size_t)bytes, cudaHostAllocDefault), "pinned alloc");
        if (with_graph) {
            cu(cudaStreamBeginCapture(s.stream, cudaStreamCaptureModeThreadLocal), "begin capture");
            mandelbrot_rgba_kernel<<<dim3((w + 15) / 16, (h + 15) / 16), dim3(16, 16), 0, s.stream>>>(
                (unsigned char*)s.dev, w, h, 1.f);
            cu(cudaGetLastError(), "captured launch");
            cu(cudaMemcpyAsync(s.host, s.dev, (std::size_t)bytes, cudaMemcpyDeviceToHost, s.stream), "captured d2h");
            cu(cudaStreamEndCapture(s.stream, &s.graph), "end capture");
            cu(cudaGraphInstantiate(&s.exec, s.graph, 0), "instantiate");
            std::size_t n = 0;
            cu(cudaGraphGetNodes(s.graph, nullptr, &n), "graph nodes");
            std::vector<cudaGraphNode_t> nodes(n);
            cu(cudaGraphGetNodes(s.graph, nodes.data(), &n), "graph nodes 2");
            for (std::size_t i = 0; i < n; ++i) {
                cudaGraphNodeType t;
                cu(cudaGraphNodeGetType(nodes[i], &t), "node type");
                if (t == cudaGraphNodeTypeKernel) s.knode = nodes[i];
            }
            ck(s.knode != nullptr, "kernel node found");
            s.kw = w; s.kh = h; s.kscale = 1.f;
            s.kargs[0] = &s.dev; s.kargs[1] = &s.kw; s.kargs[2] = &s.kh; s.kargs[3] = &s.kscale;
            s.kp.func = (void*)mandelbrot_rgba_kernel;
            s.kp.gridDim = dim3((w + 15) / 16, (h + 15) / 16);
            s.kp.blockDim = dim3(16, 16);
            s.kp.sharedMemBytes = 0;
            s.kp.kernelParams = s.kargs;
            s.kp.extra = nullptr;
        }
    }
}

void free_pooled(std::vector<slot>& slots) {
    for (auto& s : slots) {
        if (s.exec) cudaGraphExecDestroy(s.exec);
        if (s.graph) cudaGraphDestroy(s.graph);
        if (s.host) cudaFreeHost(s.host);
        if (s.dev) cudaFree(s.dev);
        if (s.ev) cudaEventDestroy(s.ev);
        if (s.stream) cudaStreamDestroy(s.stream);
    }
    slots.clear();
}

double now_seconds() {
    using clk = std::chrono::steady_clock;
    static const auto t0 = clk::now();
    return std::chrono::duration<double>(clk::now() - t0).count();
}

// Native path: identical resource lifecycle to bench/cuda/benchmark.cpp.
double run_native(int w, int h, int n, int window, checker& chk) {
    std::vector<op_state*> ops((std::size_t)window, nullptr);
    std::vector<char> busy((std::size_t)window, 0);
    int issued = 0, done = 0;
    const double t0 = now_seconds();
    while (done < n) {
        for (int i = 0; i < window && issued < n; ++i) {
            if (busy[(std::size_t)i]) continue;
            auto* st = new op_state();
            st->host.resize((std::size_t)bytes_of(w, h));
            cu(cudaStreamCreateWithFlags(&st->stream, cudaStreamNonBlocking), "stream create");
            cu(cudaEventCreateWithFlags(&st->ev, cudaEventDisableTiming), "event create");
            cu(cudaMalloc(&st->dev, st->host.size()), "device alloc");
            mandelbrot_rgba_kernel<<<dim3((w + 15) / 16, (h + 15) / 16), dim3(16, 16), 0, st->stream>>>(
                (unsigned char*)st->dev, w, h, 1.f);
            cu(cudaGetLastError(), "kernel launch");
            cu(cudaMemcpyAsync(st->host.data(), st->dev, st->host.size(), cudaMemcpyDeviceToHost, st->stream), "D2H");
            cu(cudaEventRecord(st->ev, st->stream), "record");
            ops[(std::size_t)i] = st;
            busy[(std::size_t)i] = 1;
            ++issued;
        }
        for (int i = 0; i < window; ++i) {
            if (!busy[(std::size_t)i]) continue;
            if (cudaEventQuery(ops[(std::size_t)i]->ev) != cudaSuccess) continue;
            auto* st = ops[(std::size_t)i];
            chk.accept(st->host.data());
            cudaFree(st->dev);
            cudaEventDestroy(st->ev);
            cudaStreamDestroy(st->stream);
            delete st;
            ops[(std::size_t)i] = nullptr;
            busy[(std::size_t)i] = 0;
            ++done;
        }
    }
    return now_seconds() - t0;
}

// Pooled paths: 1 = pooled+pinned, 2 = graph replay, 3 = graph replay + node param update.
double run_pooled(int w, int h, int n, int window, checker& chk, int variant) {
    std::vector<slot> slots;
    build_pooled(slots, w, h, window, variant >= 2);
    std::vector<char> busy((std::size_t)window, 0);
    int issued = 0, done = 0;
    const double t0 = now_seconds();
    while (done < n) {
        for (int i = 0; i < window && issued < n; ++i) {
            if (busy[(std::size_t)i]) continue;
            slot& s = slots[(std::size_t)i];
            if (variant == 1) {
                mandelbrot_rgba_kernel<<<dim3((w + 15) / 16, (h + 15) / 16), dim3(16, 16), 0, s.stream>>>(
                    (unsigned char*)s.dev, w, h, 1.f);
                cu(cudaGetLastError(), "kernel launch");
                cu(cudaMemcpyAsync(s.host, s.dev, (std::size_t)bytes_of(w, h), cudaMemcpyDeviceToHost, s.stream), "D2H");
            } else if (variant == 2) {
                cu(cudaGraphLaunch(s.exec, s.stream), "graph launch");
            } else {
                cu(cudaGraphExecKernelNodeSetParams(s.exec, s.knode, &s.kp), "graph set params");
                cu(cudaGraphLaunch(s.exec, s.stream), "graph launch");
            }
            cu(cudaEventRecord(s.ev, s.stream), "record");
            busy[(std::size_t)i] = 1;
            ++issued;
        }
        for (int i = 0; i < window; ++i) {
            if (!busy[(std::size_t)i]) continue;
            if (cudaEventQuery(slots[(std::size_t)i].ev) != cudaSuccess) continue;
            chk.accept(slots[(std::size_t)i].host);
            if (g_copy_result) {
                std::vector<unsigned char> materialized(slots[(std::size_t)i].host,
                                                       slots[(std::size_t)i].host + bytes_of(w, h));
                g_sink += materialized[0];
            }
            busy[(std::size_t)i] = 0;
            ++done;
        }
    }
    const double elapsed = now_seconds() - t0;
    free_pooled(slots);
    return elapsed;
}

const char* name_of(int variant) {
    switch (variant) {
        case 0: return "native";
        case 1: return "pool";
        case 2: return "graph";
        default: return "graph_param";
    }
}
} // namespace

int main() {
    g_copy_result = std::getenv("PROBE_COPY") != nullptr;
    cu(cudaFree(nullptr), "init");
    int dev = 0;
    cudaDeviceProp prop{};
    cu(cudaGetDevice(&dev), "device");
    cu(cudaGetDeviceProperties(&prop, dev), "props");
    int driver_version = 0;
    cu(cudaDriverGetVersion(&driver_version), "driver version");
    int runtime_version = 0;
    cu(cudaRuntimeGetVersion(&runtime_version), "runtime version");
    std::fprintf(stderr, "device: %s sm_%d%d driver_raw %d runtime_raw %d\n", prop.name, prop.major, prop.minor,
                 driver_version, runtime_version);

    std::printf("kind,path,width,height,window,rep,seconds,fps\n");
    for (int size : {128, 512}) {
        checker chk;
        chk.w = size;
        chk.h = size;
        const int n = size == 128 ? 6000 : 1500;
        for (int window : {1, 8}) {
            const bool native_only = std::getenv("PROBE_ONLY_NATIVE") != nullptr;
            for (int variant = 0; variant < (native_only ? 1 : 4); ++variant) {
                if (variant == 0) run_native(size, size, 8, window, chk);
                else run_pooled(size, size, 8, window, chk, variant);
            }
            for (int rep = 0; rep < 5; ++rep) {
                for (int k = 0; k < (native_only ? 1 : 4); ++k) {
                    const int variant = native_only ? 0 : (k + rep) % 4;
                    double sec = 0;
                    if (variant == 0) sec = run_native(size, size, n, window, chk);
                    else sec = run_pooled(size, size, n, window, chk, variant);
                    const double fps = n / sec;
                    std::printf("RUN,%s,%d,%d,%d,%d,%.9f,%.3f\n", name_of(variant), size, size, window, rep, sec, fps);
                    std::fflush(stdout);
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        }
        std::printf("VERIFY,%d,%d,%llu\n", size, size, (unsigned long long)chk.count);
        std::fflush(stdout);
    }
    return 0;
}
