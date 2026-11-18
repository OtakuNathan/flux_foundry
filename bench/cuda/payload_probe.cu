// Payload probe: what does the device-to-host byte count alone cost?
//
// Fixed kernel (the bench/cuda Mandelbrot, full RGBA write), pooled slots, pinned
// host buffers, one event per slot. The only variable is how many bytes are copied
// back per image. This prices the PCIe wall that caps 512x512 window 8 near
// 12,700 images/s in every pooled mode.
//
// Pixel verification is skipped when the copy is truncated: a partial image cannot
// be compared against the golden sample set. The kernel and the copy are real.
#include <cuda_runtime.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "cuda_image_kernel.cuh"

namespace {
void cu(cudaError_t c, const char* what) {
    if (c != cudaSuccess) {
        std::fprintf(stderr, "CUDA FAILED %s: %s\n", what, cudaGetErrorString(c));
        std::abort();
    }
}

struct slot {
    cudaStream_t stream = nullptr;
    cudaEvent_t ev = nullptr;
    void* dev = nullptr;
    unsigned char* pinned = nullptr;
};

double now_seconds() {
    using clk = std::chrono::steady_clock;
    static const auto t0 = clk::now();
    return std::chrono::duration<double>(clk::now() - t0).count();
}

// n images, `window` slots in flight, `copy_bytes` returned per image.
double trial(std::vector<slot>& slots, int w, int h, int n, int window, std::size_t copy_bytes,
             unsigned long long* sink) {
    std::vector<char> busy((std::size_t)window, 0);
    int issued = 0, done = 0;
    const double t0 = now_seconds();
    while (done < n) {
        for (int i = 0; i < window && issued < n; ++i) {
            if (busy[(std::size_t)i]) continue;
            slot& s = slots[(std::size_t)i];
            mandelbrot_rgba_kernel<<<dim3((w + 15) / 16, (h + 15) / 16), dim3(16, 16), 0, s.stream>>>(
                (unsigned char*)s.dev, w, h, 1.f);
            cu(cudaGetLastError(), "kernel launch");
            cu(cudaMemcpyAsync(s.pinned, s.dev, copy_bytes, cudaMemcpyDeviceToHost, s.stream), "D2H");
            cu(cudaEventRecord(s.ev, s.stream), "record");
            busy[(std::size_t)i] = 1;
            ++issued;
        }
        for (int i = 0; i < window; ++i) {
            if (!busy[(std::size_t)i]) continue;
            if (cudaEventQuery(slots[(std::size_t)i].ev) != cudaSuccess) continue;
            // Touch the transferred bytes so the copy cannot be optimised away.
            *sink += slots[(std::size_t)i].pinned[0] + slots[(std::size_t)i].pinned[copy_bytes - 1];
            busy[(std::size_t)i] = 0;
            ++done;
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
    int driver = 0, runtime = 0;
    cu(cudaDriverGetVersion(&driver), "driver");
    cu(cudaRuntimeGetVersion(&runtime), "runtime");
    std::fprintf(stderr, "device: %s sm_%d%d driver_raw %d runtime_raw %d\n", prop.name, prop.major, prop.minor,
                 driver, runtime);

    unsigned long long sink = 0;
    std::printf("kind,width,height,window,copy_bytes,rep,seconds,fps,GBps\n");
    for (int size : {128, 512}) {
        const std::size_t full = (std::size_t)size * (std::size_t)size * 4u;
        const int n = size == 128 ? 6000 : 1500;
        const int window = 8;
        std::vector<std::size_t> payloads;
        for (std::size_t div : {(std::size_t)1, (std::size_t)2, (std::size_t)4, (std::size_t)16})
            payloads.push_back(full / div);
        for (std::size_t copy_bytes : payloads) {
            std::vector<slot> slots((std::size_t)window);
            for (auto& s : slots) {
                cu(cudaStreamCreateWithFlags(&s.stream, cudaStreamNonBlocking), "stream");
                cu(cudaEventCreateWithFlags(&s.ev, cudaEventDisableTiming), "event");
                cu(cudaMalloc(&s.dev, full), "dev alloc");
                cu(cudaHostAlloc((void**)&s.pinned, full, cudaHostAllocDefault), "pinned alloc");
            }
            double warm = trial(slots, size, size, 8, window, copy_bytes, &sink);
            (void)warm;
            for (int rep = 0; rep < 5; ++rep) {
                const double sec = trial(slots, size, size, n, window, copy_bytes, &sink);
                const double fps = n / sec;
                const double gbps = fps * (double)copy_bytes / 1e9;
                std::printf("RUN,%d,%d,%d,%zu,%d,%.9f,%.3f,%.3f\n", size, size, window, copy_bytes, rep, sec, fps,
                            gbps);
                std::fflush(stdout);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            for (auto& s : slots) {
                cudaFreeHost(s.pinned);
                cudaFree(s.dev);
                cudaEventDestroy(s.ev);
                cudaStreamDestroy(s.stream);
            }
        }
    }
    std::fprintf(stderr, "sink=%llu\n", sink);
    return 0;
}
