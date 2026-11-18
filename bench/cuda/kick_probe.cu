// Kick versus poll: how should a pooled CUDA pipeline detect completion?
//
//   poll      : one disable-timing event per slot, busy cudaEventQuery in the same
//               thread that issues work. Lowest latency, burns a core.
//   hostfunc  : cudaLaunchHostFunc enqueued after each image; the driver thread runs
//               a callback that pushes the slot into a queue, and the issuing thread
//               blocks on a condition variable until an image is ready.
//
// Both do the same 257-sample verification per image. Both keep `window` images in
// flight. Reported per configuration: throughput, issuing-thread CPU, whole-process
// CPU, and the mean detection latency (issue -> the moment the completion is visible
// to the host), which is what a callback costs versus a spin.
#include <cuda_runtime.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <sys/resource.h>
#include <thread>
#include <time.h>
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

long long now_ns() {
    using clk = std::chrono::steady_clock;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now().time_since_epoch()).count();
}
double wall_seconds() { return (double)now_ns() / 1e9; }
double thread_cpu_seconds() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
double process_cpu_seconds() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return (double)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) +
           (double)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
}

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

struct completion_queue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<int> items;
    bool finished = false;
    void push(int index) {
        { std::lock_guard<std::mutex> g(m); items.push_back(index); }
        cv.notify_one();
    }
    bool pop(int& index) {
        std::unique_lock<std::mutex> g(m);
        cv.wait(g, [&] { return !items.empty() || finished; });
        if (items.empty()) return false;
        index = items.front();
        items.pop_front();
        return true;
    }
    void finish() {
        { std::lock_guard<std::mutex> g(m); finished = true; }
        cv.notify_all();
    }
};

struct slot {
    cudaStream_t stream = nullptr;
    cudaEvent_t ev = nullptr;
    void* dev = nullptr;
    unsigned char* pinned = nullptr;
    long long issue_ns = 0;
    int index = 0;
    completion_queue* q = nullptr;
    std::atomic<long long>* lat_sum = nullptr;
    std::atomic<long long>* lat_count = nullptr;
};

struct result_row {
    double fps = 0;
    double wall = 0;
    double thread_cpu = 0;
    double proc_cpu = 0;
    double mean_latency_us = 0;
};

void CUDART_CB host_completion(void* user) {
    auto* s = static_cast<slot*>(user);
    const long long delta = now_ns() - s->issue_ns;
    s->lat_sum->fetch_add(delta, std::memory_order_relaxed);
    s->lat_count->fetch_add(1, std::memory_order_relaxed);
    s->q->push(s->index);
}

void issue(slot& s, int w, int h, bool use_hostfunc) {
    s.issue_ns = now_ns();
    mandelbrot_rgba_kernel<<<dim3((w + 15) / 16, (h + 15) / 16), dim3(16, 16), 0, s.stream>>>(
        (unsigned char*)s.dev, w, h, 1.f);
    cu(cudaGetLastError(), "kernel launch");
    cu(cudaMemcpyAsync(s.pinned, s.dev, (std::size_t)w * h * 4u, cudaMemcpyDeviceToHost, s.stream), "D2H");
    if (use_hostfunc) cu(cudaLaunchHostFunc(s.stream, host_completion, &s), "host func");
    else cu(cudaEventRecord(s.ev, s.stream), "record");
}

result_row run_poll(std::vector<slot>& slots, checker& chk, int n, int w, int h) {
    const int window = (int)slots.size();
    std::vector<char> busy((std::size_t)window, 0);
    std::atomic<long long> lat_sum{0}, lat_count{0};
    int issued = 0, done = 0;
    const double wall0 = wall_seconds();
    const double cpu0 = thread_cpu_seconds();
    const double proc0 = process_cpu_seconds();
    while (done < n) {
        for (int i = 0; i < window && issued < n; ++i) {
            if (busy[(std::size_t)i]) continue;
            issue(slots[(std::size_t)i], w, h, false);
            busy[(std::size_t)i] = 1;
            ++issued;
        }
        for (int i = 0; i < window; ++i) {
            if (!busy[(std::size_t)i]) continue;
            if (cudaEventQuery(slots[(std::size_t)i].ev) != cudaSuccess) continue;
            lat_sum += now_ns() - slots[(std::size_t)i].issue_ns;
            lat_count += 1;
            chk.accept(slots[(std::size_t)i].pinned);
            busy[(std::size_t)i] = 0;
            ++done;
        }
    }
    result_row r;
    r.wall = wall_seconds() - wall0;
    r.thread_cpu = thread_cpu_seconds() - cpu0;
    r.proc_cpu = process_cpu_seconds() - proc0;
    r.fps = n / r.wall;
    r.mean_latency_us = (double)lat_sum.load() / (double)lat_count.load() / 1000.0;
    return r;
}

result_row run_hostfunc(std::vector<slot>& slots, checker& chk, int n, int w, int h) {
    const int window = (int)slots.size();
    completion_queue q;
    std::atomic<long long> lat_sum{0}, lat_count{0};
    std::vector<int> free_slots;
    for (int i = 0; i < window; ++i) free_slots.push_back(i);
    for (auto& s : slots) {
        s.q = &q;
        s.lat_sum = &lat_sum;
        s.lat_count = &lat_count;
    }
    int issued = 0, done = 0;
    const double wall0 = wall_seconds();
    const double cpu0 = thread_cpu_seconds();
    const double proc0 = process_cpu_seconds();
    while (done < n) {
        while (!free_slots.empty() && issued < n) {
            const int idx = free_slots.back();
            free_slots.pop_back();
            issue(slots[(std::size_t)idx], w, h, true);
            ++issued;
        }
        int ready = -1;
        if (q.pop(ready)) {
            chk.accept(slots[(std::size_t)ready].pinned);
            ++done;
            free_slots.push_back(ready);
        }
    }
    q.finish();
    result_row r;
    r.wall = wall_seconds() - wall0;
    r.thread_cpu = thread_cpu_seconds() - cpu0;
    r.proc_cpu = process_cpu_seconds() - proc0;
    r.fps = n / r.wall;
    r.mean_latency_us = (double)lat_sum.load() / (double)lat_count.load() / 1000.0;
    return r;
}
} // namespace

int main() {
    cu(cudaFree(nullptr), "init");
    int dev = 0;
    cudaDeviceProp prop{};
    cu(cudaGetDevice(&dev), "device");
    cu(cudaGetDeviceProperties(&prop, dev), "props");
    std::fprintf(stderr, "device: %s sm_%d%d\n", prop.name, prop.major, prop.minor);

    std::printf("kind,mode,width,window,rep,fps,wall_s,thread_cpu_s,proc_cpu_s,cpu_ratio_thread,mean_latency_us\n");
    for (int size : {128, 512}) {
        checker chk;
        chk.w = size;
        chk.h = size;
        const int n = size == 128 ? 6000 : 1500;
        for (int window : {1, 2, 4, 8}) {
            std::vector<slot> slots((std::size_t)window);
            for (int i = 0; i < window; ++i) {
                slot& s = slots[(std::size_t)i];
                s.index = i;
                cu(cudaStreamCreateWithFlags(&s.stream, cudaStreamNonBlocking), "stream");
                cu(cudaEventCreateWithFlags(&s.ev, cudaEventDisableTiming), "event");
                cu(cudaMalloc(&s.dev, (std::size_t)size * size * 4u), "dev alloc");
                cu(cudaHostAlloc((void**)&s.pinned, (std::size_t)size * size * 4u, cudaHostAllocDefault), "pinned");
            }
            for (int pass = 0; pass < 12; ++pass) {
                const bool hostfunc = (pass % 2) == 1;
                const char* mode = hostfunc ? "hostfunc" : "poll";
                const result_row r = hostfunc ? run_hostfunc(slots, chk, n, size, size)
                                              : run_poll(slots, chk, n, size, size);
                if (pass < 2) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue; // one warm-up pass per mode
                }
                std::printf("RUN,%s,%d,%d,%d,%.3f,%.6f,%.6f,%.6f,%.3f,%.3f\n", mode, size, window, (pass - 2) / 2,
                            r.fps, r.wall, r.thread_cpu, r.proc_cpu, r.thread_cpu / r.wall, r.mean_latency_us);
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
        std::printf("VERIFY,%d,%d,%llu\n", size, size, (unsigned long long)chk.count);
        std::fflush(stdout);
    }
    return 0;
}
