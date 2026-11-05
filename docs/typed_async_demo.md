# Typed external operations in an FF Blueprint

FF owns the business pipeline and its scheduling. Dynabridge is an optional tool
for declaring calls, generating typed delegates, retaining arguments and handing
off completion responsibility. It does not become an executor or a runtime above FF.

The [CPU demo](../test/typed_async_demo.cpp) runs:

```text
FF Blueprint
  -> await Host A.add(20, input)
  -> prepare Host B.multiply(result, 2)
  -> await Host B
  -> calculate result / 2
  -> deliver the result
```

Both hosts use the same backend context type. The declaration table
[`typed_async.def`](../test/typed_async.def) describes `add` and `multiply`.
The application supplies the backend and a small host loop that services accepted
operations. This is a deterministic in-process example, not a network benchmark.
Capstan supplies the separate real TCP/capability example.

The demo executes both concrete and erased delegates, reuses one Blueprint with
fresh invocations, and preserves a typed rejection when Host B has closed.
Each invocation starts once. A reusable Blueprint does not capture a single
move-only invocation for repeated submission.

## Build and run without a GPU

From the FF checkout, with Dynabridge in `../bridge`:

```sh
cmake -S . -B build-typed -DCMAKE_BUILD_TYPE=Release \
  -DFLUX_FOUNDRY_BUILD_TESTS=ON -DDYNABRIDGE_INCLUDE_DIR="$PWD/../bridge"
cmake --build build-typed --target flux_foundry_typed_async_demo \
  flux_foundry_typed_async_demo_noexc_0 flux_foundry_typed_async_demo_noexc_1 --parallel 2
ctest --test-dir build-typed -L typed --output-on-failure
```

The two no-exception targets cover relaxed (`0`) and strict (`1`) contracts on
GCC/Clang. FF has no mandatory Dynabridge dependency; these targets are created
only when the optional include directory is available.

Use `bind_static_async` / `make_invocation` to retain concrete callable types.
Use `bind_async` / `erase_invocation` when a common `invocation<R>` type is useful.
Both are consumed through the same FF awaitable integration. Overload selection,
capture, error types and completion ownership belong to the Dynabridge tool;
composition, continuations and executor selection belong to FF.

Async delegate preparation is const-correct. That does not classify a remote
method as read-only or grant any capability. The FF wrappers also support mutable
stateful callables: `then`, `on_error`, and `end` invoke their stored callable
without imposing constness. Reference capture is supported independently.
Concurrent access to captured mutable state still requires the application's
chosen synchronization.

## CUDA example

The typed CUDA example has two compilation boundaries:

- [`cuda_typed_demo.cpp`](../test/cuda_typed_demo.cpp): the FF/typed-call adapter,
  shared context, event polling and result delivery; compiled by the host compiler.
- [`cuda_typed_device.cu`](../test/cuda_typed_device.cu): kernels and native device
  operations; compiled by NVCC. The native C++ header shares typed resource state
  and owned results, without an erased C callback/context interface.

NVCC does not parse FF or Dynabridge templates. The adapter's `add_one` and `render`
operations use one context type. Normal completion polls successfully recorded
events that cover the kernel and D2H copy. A failed event record drains its stream;
it never treats an unrecorded event as proof that the kernel has finished.
Failure cleanup can block that host-loop iteration while draining the affected
stream. Normal completion polls every pending event without FIFO waiting.
Per-operation `cudaMalloc`/`cudaFree` are simple demo choices and can synchronize;
this example does not claim a nonblocking GPU allocator or GPU throughput results.

All resource release and downstream execution occur on the application's host
loop, outside CUDA host callbacks. Closing rejects new submissions and leaves
accepted operations to drain. Polling retains its context during continuations,
including when a continuation closes and drops the last external owner.

For a Turing GPU such as the RTX 2080 Ti:

```sh
cmake -S . -B build-typed-cuda -DCMAKE_BUILD_TYPE=Release \
  -DDYNABRIDGE_INCLUDE_DIR="$PWD/../bridge" \
  -DCMAKE_CUDA_COMPILER="$HOME/.local/opt/cuda-13.4.3/bin/nvcc" \
  -DCMAKE_CUDA_ARCHITECTURES=75
cmake --build build-typed-cuda --target flux_foundry_cuda_typed_demo \
  flux_foundry_cuda_typed_demo_erased flux_foundry_cuda_typed_demo_noexc_0 \
  flux_foundry_cuda_typed_demo_noexc_1 --parallel 2
ctest --test-dir build-typed-cuda -R '^cuda_typed_' --output-on-failure
```

Choose the architecture for the actual device and toolkit. Demo targets respect
`CMAKE_CUDA_ARCHITECTURES`; they no longer override it with `sm_52`.
The command above uses a user-directory installation of NVIDIA's
[official redistributable components](https://developer.download.nvidia.com/compute/cuda/redist/).
Other toolkit installations can supply their own `nvcc` path.

The eight CUDA tests cover normal and injected record-failure paths in concrete,
erased, relaxed no-exception and strict no-exception builds. They validate scalar
values, image shape/alpha/color data and close/release from a continuation.
A missing GPU returns CTest's skip code 77. Portable completion-condition tests
run separately; a skipped device test is not evidence of GPU execution.

On 2026-11-05 these tests ran on an RTX 2080 Ti with driver 615.78.08,
CUDA 13.4.92 (13.4.3 release) and GCC 16.2.1. All eight passed without skips.
The GitHub CUDA job provides compilation coverage; hosted runners without a GPU
skip the device tests. CPU typed demos are also covered by the GCC/Clang CI matrix.
