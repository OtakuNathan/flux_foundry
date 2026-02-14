#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SRC="${ROOT_DIR}/bench/horizontal_compare/compare_flux_asio_noexcept.cpp"
OUT="/tmp/flux_asio_horizontal_compare_noexcept"
ASIO_ROOT="${ASIO_ROOT:-/tmp/flux_foundry_asio}"
ASIO_DIR="${ASIO_ROOT}/asio/include"

if [[ ! -d "${ASIO_DIR}" ]]; then
  echo "[fetch] standalone Asio -> ${ASIO_ROOT}"
  git clone --depth 1 https://github.com/chriskohlhoff/asio.git "${ASIO_ROOT}"
fi

echo "[build] ${OUT}"
c++ -std=c++14 -O3 -DNDEBUG -fno-exceptions \
  -DFLUEX_FOUNDRY_NO_EXCEPTION_STRICT=1 \
  -DASIO_NO_EXCEPTIONS=1 \
  -DASIO_DISABLE_EXCEPTIONS=1 \
  -pthread \
  ${EXTRA_CXXFLAGS:-} \
  -I"${ROOT_DIR}" \
  -I"${ASIO_DIR}" \
  "${SRC}" \
  -o "${OUT}"

echo "[run] ${OUT}"
"${OUT}"
