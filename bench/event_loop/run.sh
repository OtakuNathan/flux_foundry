#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${FF_EVENT_BINARY:-/tmp/ff_event_loop}"
# gsource_executor has a throwing setup path; compare all backends with the
# same exception-enabled toolchain rather than mix unlike build modes.
"${CXX:-c++}" -std=c++14 -O3 -DNDEBUG -pthread \
  ${EXTRA_CXXFLAGS:-} -I"${ROOT}" $(pkg-config --cflags glib-2.0 libuv) \
  "${ROOT}/bench/event_loop/event_loop.cpp" -o "${OUT}" \
  $(pkg-config --libs glib-2.0 libuv)
"${OUT}" "$@"
