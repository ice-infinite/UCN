#!/bin/bash
# CLV2-14-09 clean Host resource matrix. Generated values are evidence for
# relative/static budgets only; they are not MCU linker or runtime-stack data.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/docs/results/M14"
mkdir -p "$OUT"
cd "$ROOT"

CLUSTER_TEXT_BUDGET=147456
CLUSTER_RODATA_BUDGET=16384
CLUSTER_BSS_BUDGET=128
CLUSTER_STACK_FRAME_BUDGET=2048
V3_TEXT_BUDGET=8192
V4_TEXT_BUDGET=32768
DUAL_TEXT_BUDGET=4096
EXPERIMENTAL_TEXT_BUDGET=73728

safe_clean() {
  local dir="$1"
  case "$dir" in
    "$ROOT"/build-m14-resource-*) rm -rf -- "$dir" ;;
    *) echo "unsafe resource build path: $dir" >&2; exit 2 ;;
  esac
}

section_totals() {
  size -A "$1" | awk '
    $1 ~ /^\.text/ { text += $2 }
    $1 ~ /^\.rodata/ { rodata += $2 }
    $1 ~ /^\.data/ { data += $2 }
    $1 ~ /^\.bss/ { bss += $2 }
    END { printf "%d,%d,%d,%d\n", text+0, rodata+0, data+0, bss+0 }'
}

max_stack_frame() {
  find "$1/CMakeFiles/ucn_cluster.dir" -name '*.su' -type f -print0 |
    xargs -0 awk -F '\t' '
      { value=$2; gsub(/[^0-9]/, "", value); if (value+0 > max) max=value+0 }
      END { print max+0 }'
}

assert_le() {
  local actual="$1" limit="$2" label="$3"
  if (( actual > limit )); then
    echo "resource gate FAILED: $label actual=$actual budget=$limit" >&2
    exit 1
  fi
}

build_profile() {
  local profile="$1"
  local lower="$2"
  local dir="$ROOT/build-m14-resource-$lower"
  safe_clean "$dir"
  cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DUCN_PROFILE="$profile" -DUCN_BUILD_TESTS=OFF \
    -DUCN_BUILD_SCALE_SIM=OFF -DUCN_BUILD_CLUSTER_SIM=ON \
    -DCMAKE_C_FLAGS="-fstack-usage -fno-stack-protector" >/dev/null
  cmake --build "$dir" --target ucn_cluster_sim --parallel >/dev/null
  local sections object_bytes stack_frame
  sections="$(section_totals "$dir/libucn_cluster.a")"
  object_bytes="$("$dir/ucn_cluster_sim" --nodes 8 --scenario clean |
    sed -n 's/.*object_bytes=\([0-9][0-9]*\).*/\1/p')"
  stack_frame="$(max_stack_frame "$dir")"
  echo "$lower,$object_bytes,$sections,$stack_frame" > "$OUT/.m14-resource-$lower.csv"
}

build_profile FULL full & full_pid=$!
build_profile LITE lite & lite_pid=$!
build_profile NANO nano & nano_pid=$!
wait "$full_pid"
wait "$lite_pid"
wait "$nano_pid"

feature_dir="$ROOT/build-m14-resource-features"
safe_clean "$feature_dir"
cmake -S . -B "$feature_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DUCN_PROFILE=FULL -DUCN_BUILD_TESTS=OFF -DUCN_BUILD_SCALE_SIM=OFF \
  -DUCN_BUILD_CLUSTER_SIM=OFF -DUCN_CLUSTER_ENABLE_V3_COMPAT=ON \
  -DUCN_BUILD_CLUSTER_TAKEOVER_EXPERIMENTAL=ON \
  -DUCN_BUILD_CLUSTER_HANDOVER_EXPERIMENTAL=ON \
  -DUCN_BUILD_CLUSTER_REKEY_EXPERIMENTAL=ON >/dev/null
cmake --build "$feature_dir" --parallel --target \
  ucn_cluster_wire_dual_stack ucn_cluster_takeover_experimental \
  ucn_cluster_handover_experimental ucn_cluster_rekey_experimental >/dev/null

core_dir="$ROOT/build-m14-resource-core-only"
safe_clean "$core_dir"
cmake -S . -B "$core_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DUCN_PROFILE=NANO -DUCN_BUILD_TESTS=OFF -DUCN_BUILD_SCALE_SIM=OFF \
  -DUCN_BUILD_CLUSTER_SIM=OFF -DUCN_FEATURE_SERVICE=OFF >/dev/null
cmake --build "$core_dir" --parallel --target ucn_core >/dev/null
if [[ -f "$core_dir/libucn_cluster.a" ]]; then
  echo "resource gate FAILED: core-only build paid Cluster archive" >&2
  exit 1
fi

v3="$(section_totals "$feature_dir/libucn_cluster_wire_v3_compat.a")"
v4="$(section_totals "$feature_dir/libucn_cluster_wire_v4.a")"
dual="$(section_totals "$feature_dir/libucn_cluster_wire_dual_stack.a")"
takeover="$(section_totals "$feature_dir/libucn_cluster_takeover_experimental.a")"
handover="$(section_totals "$feature_dir/libucn_cluster_handover_experimental.a")"
rekey="$(section_totals "$feature_dir/libucn_cluster_rekey_experimental.a")"

{
  echo "profile,object_bytes,text,rodata,data,bss,max_static_stack_frame"
  cat "$OUT/.m14-resource-full.csv"
  cat "$OUT/.m14-resource-lite.csv"
  cat "$OUT/.m14-resource-nano.csv"
} > "$OUT/cluster_resource_profiles_2026-08-25.csv"

{
  echo "module,text,rodata,data,bss"
  echo "wire_v3,$v3"
  echo "wire_v4,$v4"
  echo "wire_dual_dispatch,$dual"
  echo "takeover_experimental,$takeover"
  echo "handover_experimental,$handover"
  echo "rekey_experimental,$rekey"
} > "$OUT/cluster_resource_modules_2026-08-25.csv"

while IFS=, read -r profile object text rodata data bss stack; do
  [[ "$profile" == "profile" ]] && continue
  assert_le "$object" 2048 "$profile Cluster object"
  assert_le "$text" "$CLUSTER_TEXT_BUDGET" "$profile Cluster text"
  assert_le "$rodata" "$CLUSTER_RODATA_BUDGET" "$profile Cluster rodata"
  assert_le "$bss" "$CLUSTER_BSS_BUDGET" "$profile Cluster bss"
  assert_le "$stack" "$CLUSTER_STACK_FRAME_BUDGET" "$profile max static frame"
done < "$OUT/cluster_resource_profiles_2026-08-25.csv"

IFS=, read -r v3_text _ <<< "$v3"
IFS=, read -r v4_text _ <<< "$v4"
IFS=, read -r dual_text _ <<< "$dual"
assert_le "$v3_text" "$V3_TEXT_BUDGET" "v3 compatibility text"
assert_le "$v4_text" "$V4_TEXT_BUDGET" "v4 codec text"
assert_le "$dual_text" "$DUAL_TEXT_BUDGET" "dual dispatcher text"
experimental_text=0
for row in "$takeover" "$handover" "$rekey"; do
  IFS=, read -r text _ <<< "$row"
  experimental_text=$((experimental_text + text))
done
assert_le "$experimental_text" "$EXPERIMENTAL_TEXT_BUDGET" \
  "all default-OFF experimental text"

rm -f "$OUT"/.m14-resource-*.csv
echo "M14 resource gate PASSED"
cat "$OUT/cluster_resource_profiles_2026-08-25.csv"
cat "$OUT/cluster_resource_modules_2026-08-25.csv"
