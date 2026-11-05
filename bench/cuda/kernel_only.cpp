// Device-only reference for the CUDA demo's render kernel. No FF/DB or D2H in timed interval.
#include "cuda_typed_device.h"
#include <cuda_runtime_api.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
static void cu(cudaError_t e,const char* what){if(e!=cudaSuccess){std::fprintf(stderr,"%s: %s\n",what,cudaGetErrorString(e));std::abort();}}
void run(int size,int rep) {
    typed_cuda::resources r;
    r.host.resize(std::size_t(size)*size*4);
    cu(cudaStreamCreateWithFlags(&r.stream,cudaStreamNonBlocking),"stream");
    cu(cudaMalloc(&r.device,r.host.size()),"malloc");
    cudaEvent_t start{},end{}; cu(cudaEventCreate(&start),"start event");cu(cudaEventCreate(&end),"end event");
    for(int i=0;i<100;++i)typed_cuda::launch_render(r,size,size);
    cu(cudaGetLastError(),"warmup launch");cu(cudaStreamSynchronize(r.stream),"warmup wait");
    const int n=size==128?30000:20000;
    cu(cudaEventRecord(start,r.stream),"record start");
    for(int i=0;i<n;++i)typed_cuda::launch_render(r,size,size);
    cu(cudaGetLastError(),"timed launch");cu(cudaEventRecord(end,r.stream),"record end");cu(cudaEventSynchronize(end),"sync");
    float ms=0;cu(cudaEventElapsedTime(&ms,start,end),"elapsed");
    cu(cudaMemcpy(r.host.data(),r.device,r.host.size(),cudaMemcpyDeviceToHost),"D2H verify");
    bool colored=false;
    for(std::size_t i=0;i<r.host.size();i+=4){if(r.host[i+3]!=255) std::abort();colored|=r.host[i]||r.host[i+1]||r.host[i+2];}
    if(!colored)std::abort();
    const double fps=1000.0*n/ms;
    std::printf("GPU_ONLY,%d,%d,%d,%d,%.3f,%.3f,%.3f\n",size,size,rep,n,ms,fps,fps*size*size/1e6);std::fflush(stdout);
    cu(cudaEventDestroy(start),"destroy start");cu(cudaEventDestroy(end),"destroy end");
}
int main(){cu(cudaFree(nullptr),"init");std::puts("kind,width,height,repeat,iterations,gpu_ms,fps,MPixels_per_second");for(int i=0;i<3;++i){if(i%2==0){run(128,i);run(512,i);}else{run(512,i);run(128,i);}}}
