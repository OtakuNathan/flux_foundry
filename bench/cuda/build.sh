#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
: "${FF:?Set FF to a checkout of flux_foundry at d1cfad070ed8bf667a0c0aff77977d7741d107fd}"
: "${DB:?Set DB to a checkout of dynabridge at 84725ad350baf56c3cddb69aa934bf9eaabf3123}"
CUDA=${CUDA:-"$HOME/.local/opt/cuda-13.4.3"}
test "$(git -C "$FF" rev-parse HEAD)" = d1cfad070ed8bf667a0c0aff77977d7741d107fd
test "$(git -C "$DB" rev-parse HEAD)" = 84725ad350baf56c3cddb69aa934bf9eaabf3123
"$CUDA/bin/nvcc" -std=c++14 -O3 -DNDEBUG --allow-unsupported-compiler -arch=sm_75 -I"$CUDA/include" -c cuda_typed_device.cu -o cuda_typed_device.o
c++ -std=c++14 -O3 -DNDEBUG -I"$FF" -I. -I"$DB" -I"$CUDA/include" benchmark.cpp cuda_typed_device.o -L"$CUDA/lib64" -Wl,-rpath,"$CUDA/lib64" -lcudart -pthread -o benchmark-rebuilt
c++ -std=c++14 -O3 -DNDEBUG -I. -I"$CUDA/include" kernel_only.cpp cuda_typed_device.o -L"$CUDA/lib64" -Wl,-rpath,"$CUDA/lib64" -lcudart -o kernel_only-rebuilt
printf 'Built benchmark-rebuilt and kernel_only-rebuilt.\n'
