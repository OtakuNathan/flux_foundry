#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SRC="${ROOT_DIR}/bench/horizontal_compare/compare_flux_asio.cpp"
OUT="/tmp/flux_asio_horizontal_compare"
ASIO_ROOT="${ASIO_ROOT:-/tmp/flux_foundry_asio}"
ASIO_DIR="${ASIO_ROOT}/asio/include"

if [[ ! -d "${ASIO_DIR}" ]]; then
  echo "[fetch] standalone Asio -> ${ASIO_ROOT}"
  git clone --depth 1 https://github.com/chriskohlhoff/asio.git "${ASIO_ROOT}"
fi

echo "[build] ${OUT}"
c++ -std=c++14 -O3 -DNDEBUG -pthread \
  -I"${ROOT_DIR}" \
  -I"${ASIO_DIR}" \
  "${SRC}" \
  -o "${OUT}"

echo "[run] ${OUT}"
"${OUT}"
