#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_root="${1:-/tmp/ucn-v6-validation}"

cmake -S "$repo_root" -B "$build_root/asan-ubsan" -G Ninja \
  -DUCN_BUILD_TESTS=ON -DUCN_BUILD_V6_EXPERIMENTAL=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build "$build_root/asan-ubsan" -j4
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir "$build_root/asan-ubsan" -R '^(ucn_v6_|v6_)' \
  --output-on-failure

cmake -S "$repo_root" -B "$build_root/analyzer" -G Ninja \
  -DUCN_BUILD_TESTS=ON -DUCN_BUILD_V6_EXPERIMENTAL=ON \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-fanalyzer"
cmake --build "$build_root/analyzer" -j4
ctest --test-dir "$build_root/analyzer" -R '^(ucn_v6_|v6_)' \
  --output-on-failure

echo "V6 sanitizer/analyzer matrix completed at $build_root"
