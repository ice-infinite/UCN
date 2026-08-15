#!/bin/bash
# CLV2-M00-05 resource baseline report (CLV2-M00.1).
#
# Uses the build trees produced by tools/m00_matrix.sh (full/lite/nano/asan/
# core_only) plus one dedicated -fstack-usage build.  Reports per-Profile
# sizeof(ucn_cluster_t), .text/.rodata/.data/.bss of libucn_cluster.a and
# the maximum static stack frame of ucn_cluster.c.
#
# Output: docs/results/m00_matrix/size_report.md
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/docs/results/m00_matrix"
mkdir -p "$OUT"
cd "$ROOT"

size_of_archive() {
  local lib="$1"
  size -A "$lib" 2>/dev/null | awk \
    '$1 ~ /^\.(text|rodata|data|bss)$/ { sum[$1] += $2 } \
     END { printf "%d %d %d %d\n", sum[".text"]+0, sum[".rodata"]+0, \
            sum[".data"]+0, sum[".bss"]+0 }'
}

ensure_stack_build() {
  if [[ ! -f build-m00-stack/CMakeCache.txt ]]; then
    cmake -S . -B build-m00-stack -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DUCN_PROFILE=FULL -DCMAKE_C_FLAGS="-fstack-usage -fno-stack-protector" \
      -DUCN_BUILD_SCALE_SIM=OFF -DUCN_BUILD_CLUSTER_SIM=OFF
    cmake --build build-m00-stack --target ucn_cluster --parallel
  fi
}

max_stack_of() {
  local su="$1"
  if [[ ! -f "$su" ]]; then
    echo "0"
    return 0
  fi
  awk -F'\t' '{ gsub(/[^0-9]/, "", $2); if ($2+0 > max) max = $2+0 } \
      END { print max+0 }' "$su"
}

echo "# Cluster Resource Baseline (CLV2-M00-05, CLV2-M00.1)" > "$OUT/size_report.md"
echo "" >> "$OUT/size_report.md"
echo "> Host x64 Debug, gcc -O0-ish profile as built by tools/m00_matrix.sh." >> "$OUT/size_report.md"
echo "> Max stack = largest STATIC frame in ucn_cluster.c under -fstack-usage (dynamic call-chain depth is not summed; a note for M01/M02: keep per-frame growth bounded)." >> "$OUT/size_report.md"
echo "" >> "$OUT/size_report.md"
echo "| Profile | sizeof(ucn_cluster_t) | .text | .rodata | .data | .bss | max static stack |" >> "$OUT/size_report.md"
echo "|---|---|---|---|---|---|---|" >> "$OUT/size_report.md"

ensure_stack_build
SU_FILE="$(find build-m00-stack -name 'ucn_cluster.c.su' | head -1)"
MAX_STACK="$(max_stack_of "$SU_FILE")"

for profile in full lite nano; do
  lib="build-m00-$profile/libucn_cluster.a"
  if [[ ! -f "$lib" ]]; then
    echo "[size-report] missing $lib; run tools/m00_matrix.sh first" >&2
    exit 1
  fi
  read -r text rodata data bss < <(size_of_archive "$lib")
  sizeof_cluster="$(./build-m00-$profile/ucn_tests 2>/dev/null | grep -o 'cluster_bytes=[0-9]*' | head -1 | cut -d= -f2)"
  sizeof_cluster="${sizeof_cluster:-0}"
  echo "| $profile | $sizeof_cluster | $text | $rodata | $data | $bss | $MAX_STACK |" >> "$OUT/size_report.md"
done

core_lib="build-m00-core_only/libucn_core.a"
if [[ -f "$core_lib" ]]; then
  read -r text rodata data bss < <(size_of_archive "$core_lib")
  echo "| CORE_ONLY (ucn_core only) | n/a (cluster not linked) | $text | $rodata | $data | $bss | n/a |" >> "$OUT/size_report.md"
  if [[ -f build-m00-core_only/libucn_cluster.a ]]; then
    echo "| CORE_ONLY cluster check | **FAIL: libucn_cluster.a present** |" >> "$OUT/size_report.md"
    echo "[size-report] core-only build contains libucn_cluster.a" >&2
    exit 1
  else
    echo "| CORE_ONLY cluster check | libucn_cluster.a absent (Cluster OFF pays nothing) |" >> "$OUT/size_report.md"
  fi
else
  echo "[size-report] missing $core_lib; run tools/m00_matrix.sh first" >&2
  exit 1
fi

echo "[size-report] wrote $OUT/size_report.md"
cat "$OUT/size_report.md"
