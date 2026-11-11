#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
: "${FF:?Set FF to a checkout of flux_foundry at 98d73c280667226f0fbb6f4408626ab875e2bd2a}"
: "${PORTAL:?Set PORTAL to a checkout of portal at 542bf5bbf6244f2a3948f7f01aa22a6233fba5fe}"
CUDA=${CUDA:-"$HOME/.local/opt/cuda-13.4.3"}
test "$(git -C "$FF" rev-parse HEAD)" = 98d73c280667226f0fbb6f4408626ab875e2bd2a
test "$(git -C "$PORTAL" rev-parse HEAD)" = 542bf5bbf6244f2a3948f7f01aa22a6233fba5fe
"$CUDA/bin/nvcc" -std=c++14 -O3 -DNDEBUG --allow-unsupported-compiler -arch=sm_75 -I"$CUDA/include" -c cuda_typed_device.cu -o cuda_typed_device.o
c++ -std=c++14 -O3 -DNDEBUG -I"$FF" -I. -I"$PORTAL" -I"$CUDA/include" benchmark.cpp cuda_typed_device.o -L"$CUDA/lib64" -Wl,-rpath,"$CUDA/lib64" -lcudart -pthread -o benchmark-rebuilt
c++ -std=c++14 -O3 -DNDEBUG -I. -I"$CUDA/include" kernel_only.cpp cuda_typed_device.o -L"$CUDA/lib64" -Wl,-rpath,"$CUDA/lib64" -lcudart -o kernel_only-rebuilt
printf 'Built benchmark-rebuilt and kernel_only-rebuilt.\n'
