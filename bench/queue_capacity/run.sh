#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${FF_LOAD_BINARY:-/tmp/ff_queue_capacity}"
"${CXX:-c++}" -std=c++14 -O3 -DNDEBUG -fno-exceptions -fno-rtti \
  -DFLUX_FOUNDRY_NO_EXCEPTION_STRICT=1 -pthread \
  ${EXTRA_CXXFLAGS:-} -I"${ROOT}" \
  "${ROOT}/bench/queue_capacity/queue_capacity.cpp" -o "${OUT}"
"${OUT}" "$@"
