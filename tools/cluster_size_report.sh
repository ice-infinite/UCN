#!/bin/bash
# CLV2-M00-05 resource baseline report (CLV2-M00.2: clean per-profile).
#
# 1) Reads sizeof/.text/.rodata/.data/.bss from the matrix build trees
#    produced by tools/m00_matrix.sh (full/lite/nano) plus the core_only
#    tree.
# 2) Builds THREE dedicated -fstack-usage trees (one per Profile) from
#    scratch (rm -rf) so every Profile reports its OWN measured max
#    static stack frame, never a value copied from FULL.
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

# Clean -fstack-usage build for one Profile; prints the max static frame
# of ucn_cluster.c (measured, not assumed).
build_stack_profile() {
  local profile="$1"
  local dir="build-m00-stack-$profile"
  local su

  rm -rf "$dir"
  cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DUCN_PROFILE="$profile" \
    -DCMAKE_C_FLAGS="-fstack-usage -fno-stack-protector" \
    -DUCN_BUILD_SCALE_SIM=OFF -DUCN_BUILD_CLUSTER_SIM=OFF > /dev/null
  cmake --build "$dir" --target ucn_cluster --parallel > /dev/null
  su="$(find "$dir" -name 'ucn_cluster.c.su' | head -1)"
  if [[ ! -f "$su" ]]; then
    echo "[size-report] missing .su for $profile" >&2
    exit 1
  fi
  awk -F'\t' '{ gsub(/[^0-9]/, "", $2); if ($2+0 > max) max = $2+0 } \
      END { print max+0 }' "$su"
}

echo "# Cluster Resource Baseline (CLV2-M00-05, CLV2-M00.2 clean)" > "$OUT/size_report.md"
echo "" >> "$OUT/size_report.md"
echo "> Host x64 Debug, gcc. Size rows come from the clean matrix trees (tools/m00_matrix.sh)." >> "$OUT/size_report.md"
echo "> Max stack = largest STATIC frame in ucn_cluster.c, MEASURED PER PROFILE in a clean -fstack-usage tree (dynamic call-chain depth is not summed; M01/M02 must keep per-frame growth bounded)." >> "$OUT/size_report.md"
echo "" >> "$OUT/size_report.md"
echo "| Profile | sizeof(ucn_cluster_t) | .text | .rodata | .data | .bss | max static stack (own clean tree) |" >> "$OUT/size_report.md"
echo "|---|---|---|---|---|---|---|" >> "$OUT/size_report.md"

# Per-profile stack trees run concurrently; each result lands in a file.
build_stack_profile FULL > /tmp/stack_full.txt & stacks_full_pid=$!
build_stack_profile LITE > /tmp/stack_lite.txt & stacks_lite_pid=$!
build_stack_profile NANO > /tmp/stack_nano.txt & stacks_nano_pid=$!
wait "$stacks_full_pid"
wait "$stacks_lite_pid"
wait "$stacks_nano_pid"
stack_full="$(cat /tmp/stack_full.txt)"
stack_lite="$(cat /tmp/stack_lite.txt)"
stack_nano="$(cat /tmp/stack_nano.txt)"
rm -f /tmp/stack_full.txt /tmp/stack_lite.txt /tmp/stack_nano.txt

for profile in full lite nano; do
  lib="build-m00-$profile/libucn_cluster.a"
  if [[ ! -f "$lib" ]]; then
    echo "[size-report] missing $lib; run tools/m00_matrix.sh first" >&2
    exit 1
  fi
  read -r text rodata data bss < <(size_of_archive "$lib" )
  sizeof_cluster="$(./build-m00-$profile/ucn_tests 2>/dev/null | grep -o 'cluster_bytes=[0-9]*' | head -1 | cut -d= -f2)"
  sizeof_cluster="${sizeof_cluster:-0}"
  case "$profile" in
    full) stack="$stack_full";;
    lite) stack="$stack_lite";;
    nano) stack="$stack_nano";;
  esac
  echo "| $profile | $sizeof_cluster | $text | $rodata | $data | $bss | $stack |" >> "$OUT/size_report.md"
done

core_lib="build-m00-core_only/libucn_core.a"
if [[ -f "$core_lib" ]]; then
  read -r text rodata data bss < <(size_of_archive "$core_lib" )
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
