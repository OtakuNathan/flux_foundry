// Completion probe: what does per-operation completion detection cost?
//
// Same kernel, pooled slots, pinned host buffers, window 8. Only the completion
// mechanism changes:
//   event   : one disable-timing event per image, cudaEventQuery in the poll loop
//   streamq : no event at all, cudaStreamQuery on the slot's stream
//   batch4  : four images per slot per round, one event recorded after the fourth
//
// Verified with the same 257 sampled pixels per image, so batch4 does the same
// per-image work as the others; only the bookkeeping is amortised.
#include <cuda_runtime.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

double now_seconds() {
    using clk = std::chrono::steady_clock;
    static const auto t0 = clk::now();
    return std::chrono::duration<double>(clk::now() - t0).count();
}

enum completion_mode { M_EVENT = 0, M_STREAMQ = 1, M_BATCH4 = 2 };
const char* mode_name(completion_mode m) {
    switch (m) {
        case M_EVENT: return "event";
        case M_STREAMQ: return "streamq";
        default: return "batch4";
    }
}

struct slot {
    cudaStream_t stream = nullptr;
    cudaEvent_t ev = nullptr;
    std::vector<void*> dev;
    std::vector<unsigned char*> pinned;
    int pending = 0;   // images issued and not yet collected
    int filled = 0;    // buffers holding completed images
};

double trial(std::vector<slot>& slots, checker& chk, int n, completion_mode mode, int w, int h) {
    const int window = (int)slots.size();
    const std::size_t bytes = (std::size_t)w * h * 4u;
    const int depth = mode == M_BATCH4 ? 4 : 1;
    int issued = 0, done = 0;
    const double t0 = now_seconds();
    while (done < n) {
        for (int i = 0; i < window && issued < n; ++i) {
            slot& s = slots[(std::size_t)i];
            if (s.pending != 0) continue;
            const int k = std::min(depth, n - issued);
            for (int j = 0; j < k; ++j) {
                mandelbrot_rgba_kernel<<<dim3((w + 15) / 16, (h + 15) / 16), dim3(16, 16), 0, s.stream>>>(
                    (unsigned char*)s.dev[(std::size_t)j], w, h, 1.f);
                cu(cudaGetLastError(), "kernel launch");
                cu(cudaMemcpyAsync(s.pinned[(std::size_t)j], s.dev[(std::size_t)j], bytes, cudaMemcpyDeviceToHost,
                                   s.stream),
                   "D2H");
            }
            if (mode != M_STREAMQ) cu(cudaEventRecord(s.ev, s.stream), "record");
            s.pending = k;
            issued += k;
        }
        for (int i = 0; i < window; ++i) {
            slot& s = slots[(std::size_t)i];
            if (s.pending == 0) continue;
            const cudaError_t ready = mode == M_STREAMQ ? cudaStreamQuery(s.stream) : cudaEventQuery(s.ev);
            if (ready != cudaSuccess) continue;
            for (int j = 0; j < s.pending; ++j) chk.accept(s.pinned[(std::size_t)j]);
            done += s.pending;
            s.pending = 0;
        }
    }
    return now_seconds() - t0;
}
} // namespace

int main() {
    cu(cudaFree(nullptr), "init");
    int dev = 0;
    cudaDeviceProp prop{};
    cu(cudaGetDevice(&dev), "device");
    cu(cudaGetDeviceProperties(&prop, dev), "props");
    std::fprintf(stderr, "device: %s sm_%d%d\n", prop.name, prop.major, prop.minor);

    std::printf("kind,mode,width,height,window,rep,seconds,fps\n");
    for (int size : {128, 512}) {
        checker chk;
        chk.w = size;
        chk.h = size;
        const int n = size == 128 ? 6000 : 1500;
        const std::size_t bytes = (std::size_t)size * size * 4u;
        for (completion_mode mode : {M_EVENT, M_STREAMQ, M_BATCH4}) {
            const int window = 8;
            const int depth = mode == M_BATCH4 ? 4 : 1;
            std::vector<slot> slots((std::size_t)window);
            for (auto& s : slots) {
                cu(cudaStreamCreateWithFlags(&s.stream, cudaStreamNonBlocking), "stream");
                if (mode != M_STREAMQ) cu(cudaEventCreateWithFlags(&s.ev, cudaEventDisableTiming), "event");
                for (int j = 0; j < depth; ++j) {
                    void* d = nullptr;
                    unsigned char* p = nullptr;
                    cu(cudaMalloc(&d, bytes), "dev alloc");
                    cu(cudaHostAlloc((void**)&p, bytes, cudaHostAllocDefault), "pinned alloc");
                    s.dev.push_back(d);
                    s.pinned.push_back(p);
                }
            }
            (void)trial(slots, chk, 8, mode, size, size); // warm
            for (int rep = 0; rep < 5; ++rep) {
                const double sec = trial(slots, chk, n, mode, size, size);
                const double fps = n / sec;
                std::printf("RUN,%s,%d,%d,%d,%d,%.9f,%.3f\n", mode_name(mode), size, size, window, rep, sec, fps);
                std::fflush(stdout);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            for (auto& s : slots) {
                for (auto* p : s.pinned) cudaFreeHost(p);
                for (auto* d : s.dev) cudaFree(d);
                if (s.ev) cudaEventDestroy(s.ev);
                cudaStreamDestroy(s.stream);
            }
        }
        std::printf("VERIFY,%d,%d,%llu\n", size, size, (unsigned long long)chk.count);
        std::fflush(stdout);
    }
    return 0;
}
