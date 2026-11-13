# Exception-enabled MSan runtime recheck

Host: Ryzen 3700X desktop, Clang 23.1.4. The initial system-libstdc++ run
reported uninitialized `std::exception_ptr` storage, reproducible without FF.
The fix is an instrumented C++ runtime for these tests, with no FF error-policy
changes or suppressions.

An isolated static libc++ / libc++abi build uses LLVM tag `llvmorg-20.1.8`,
commit `87f0227cb60147a26a1eeb4fb06e3b505e9c7261`. The existing compiler builds
both libraries with MSan and origin tracking. Nothing is installed system-wide.

From the desktop benchmark scratch checkout:

```sh
mkdir -p build/msan-runtime
git clone --depth 1 --filter=blob:none --sparse --branch llvmorg-20.1.8 \
  https://github.com/llvm/llvm-project.git build/msan-runtime/source
git -C build/msan-runtime/source sparse-checkout set \
  libc libcxx libcxxabi libunwind runtimes cmake llvm/cmake \
  llvm/utils/llvm-lit llvm/utils/lit
cmake -S build/msan-runtime/source/runtimes -B build/msan-runtime/build \
  -G 'Unix Makefiles' -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  '-DLLVM_ENABLE_RUNTIMES=libcxx;libcxxabi' \
  -DLLVM_USE_SANITIZER=MemoryWithOrigins -DLLVM_INCLUDE_TESTS=OFF \
  -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
  -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXXABI_ENABLE_SHARED=OFF \
  -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/msan-runtime/install"
cmake --build build/msan-runtime/build -j 6
cmake --install build/msan-runtime/build
```

The sparse source list includes LLVM libc's shared floating-point support
headers used by libc++; libc itself is not built. The initial incomplete
sparse checkout was corrected before the successful runtime build. Build and
configure logs remain in the desktop scratch directory.

Recheck commands below use Bash arrays. `build/batch-prefix-final/{before,after}`
are the comparison overlays; both contain the optimized runner factory, and
only the packet-batch implementation differs. The standalone sources stored
beside this report were copied to `build/batch-prefix-tests/` for execution.

```bash
prefix="$PWD/build/msan-runtime/install"
flags=(-std=c++14 -O1 -g -pthread -Wall -Wextra -Werror
  -fsanitize=memory -fsanitize-memory-track-origins=2
  -fno-omit-frame-pointer -fPIE -pie -stdlib=libc++ -nostdinc++
  -isystem "$prefix/include/c++/v1" -L"$prefix/lib")
clang++ "${flags[@]}" build/batch-prefix-tests/msan-exception-minimal.cpp \
  -lc++abi -o build/batch-prefix-tests/msan-libcxx-minimal
build/batch-prefix-tests/msan-libcxx-minimal

export PKG_CONFIG_PATH="$PWD/deps/usr/lib/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$PWD/deps"
read -r -a dpdk_flags <<< "$(pkg-config --cflags libdpdk)"
for version in before after; do
  clang++ "${flags[@]}" -I"$PWD/build/batch-prefix-final/$version" -I"$PWD" \
    "${dpdk_flags[@]}" test/dpdk_awaitable_contract.cpp -lc++abi \
    -o "build/batch-prefix-tests/msan-libcxx-$version"
  "build/batch-prefix-tests/msan-libcxx-$version"
done
```

| Program | Runtime | Exit | Result |
|---|---|---:|---|
| Minimal `make_exception_ptr` | System libstdc++ | 1 | MSan uninitialized-value report |
| Before contract | System libstdc++ | 1 | Same exception_ptr boundary |
| After contract | System libstdc++ | 1 | Same exception_ptr boundary |
| Minimal `make_exception_ptr` | Instrumented libc++ / libc++abi | 0 | Pass; empty log |
| Before contract | Instrumented libc++ / libc++abi | 0 | Pass, including cancellation |
| After contract | Instrumented libc++ / libc++abi | 0 | Pass, including cancellation |
| Deliberate heap-uninitialized read | Instrumented libc++ / libc++abi | 1 | Expected MSan report |

Logs: [minimal](msan-libcxx-minimal.log), [before](msan-libcxx-before.log),
[after](msan-libcxx-after.log), [negative control](msan-negative.log).
The [negative-control source](msan-negative.cpp) was built with the same
compiler, runtime, C++14, `-O1 -g`, MSan/origin and PIE options. It uses a volatile
read of a freshly allocated, uninitialized `int` so the read remains observable.
The expected diagnostic verifies that the runtime configuration still detects
uninitialized values.

This validates FF with the instrumented fake DPDK backend. Native DPDK runtime
calls, NIC DMA and native io_uring memory writes are not exercised by this test.
