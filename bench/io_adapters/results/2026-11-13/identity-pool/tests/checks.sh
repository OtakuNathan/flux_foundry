#!/usr/bin/env bash
set -euo pipefail
root=${1:?source directory required}
cd "$root"
for kind in asan tsan; do
 flags=(-fsanitize=thread -fno-omit-frame-pointer)
 if [[ "$kind" == asan ]]; then flags=(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer); fi
 for src in identity_pool_contract awaitable_lifecycle_contract pipeline_lifetime_contract; do
  clang++ -std=c++14 -O1 -g -pthread -Wall -Wextra -Werror -I. "${flags[@]}" "test/$src.cpp" -o "$src-$kind"
  TSAN_OPTIONS='halt_on_error=1 exitcode=66' ASAN_OPTIONS='detect_leaks=1:halt_on_error=1' timeout 90 "./$src-$kind" > "$src-$kind.log" 2>&1
  tail -1 "$src-$kind.log"
 done
done
