#!/bin/bash
# CLV2-M00 gate matrix: 6 independent fresh builds, fail-closed.
#
# Usage:
#   bash tools/m00_matrix.sh                # canonical CLEAN run: every
#                                          # build dir is rm -rf'ed and
#                                          # rebuilt from zero
#   bash tools/m00_matrix.sh --incremental # developer mode: reuse trees
#                                          # (its logs are NOT valid
#                                          # audit evidence)
#
# Exit 0 only if EVERY route configured, built and passed ctest.
# Any configure/build/ctest failure (or a missing core-only assertion)
# makes the whole script exit non-zero.
#
# Routes: full, lite, nano, asan, analyzer, core_only.
# Logs land in docs/results/m00_matrix/m00-<name>.log (overwritten per run).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/docs/results/m00_matrix"
mkdir -p "$OUT"
cd "$ROOT"

INCREMENTAL=0
if [[ "${1:-}" == "--incremental" ]]; then
  INCREMENTAL=1
fi

GEN="-G Ninja -DCMAKE_BUILD_TYPE=Debug"

prepare_dir() {
  local dir="$1"
  if [[ "$INCREMENTAL" -eq 0 ]]; then
    rm -rf "$dir"
  fi
}

run_one() {
  local name="$1"
  shift
  local dir="build-m00-$name"
  local log="$OUT/m00-$name.log"
  prepare_dir "$dir"
  {
    if [[ "$INCREMENTAL" -eq 0 ]]; then
      echo "==== [$name] configure (clean tree) ===="
    else
      echo "==== [$name] configure (incremental) ===="
    fi
    cmake -S . -B "$dir" $GEN "$@"
    if [[ "$INCREMENTAL" -eq 0 ]]; then
      echo "==== [$name] build (clean tree) ===="
    else
      echo "==== [$name] build (incremental) ===="
    fi
    cmake --build "$dir" --parallel
    echo "==== [$name] ctest ===="
    ctest --test-dir "$dir" --output-on-failure
  } > "$log" 2>&1
  # rc of the { } block above = rc of its last command (ctest).
}

run_core_only() {
  local dir="build-m00-core_only"
  local log="$OUT/m00-core_only.log"
  prepare_dir "$dir"
  {
    echo "==== [core_only] configure ===="
    cmake -S . -B "$dir" $GEN \
      -DUCN_BUILD_TESTS=OFF -DUCN_BUILD_SCALE_SIM=OFF \
      -DUCN_BUILD_CLUSTER_SIM=OFF -DUCN_FEATURE_SERVICE=OFF
    echo "==== [core_only] build ===="
    cmake --build "$dir" --target ucn_core --parallel
    echo "==== [core_only] assert cluster not linked ===="
    if [[ -f "$dir/libucn_cluster.a" ]]; then
      echo "[core_only] FAIL: libucn_cluster.a exists in a core-only build"
      return 1
    fi
    echo "[core_only] OK: libucn_cluster.a absent (Cluster OFF pays nothing)"
  } > "$log" 2>&1
}

names=(full lite nano asan analyzer core_only)
pids=()

run_one full -DUCN_PROFILE=FULL & pids+=("$!")
run_one lite -DUCN_PROFILE=LITE & pids+=("$!")
run_one nano -DUCN_PROFILE=NANO & pids+=("$!")
run_one asan -DUCN_PROFILE=FULL \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" & pids+=("$!")
run_one analyzer -DUCN_PROFILE=FULL -DCMAKE_C_FLAGS="-fanalyzer" & pids+=("$!")
run_core_only & pids+=("$!")

status=0
for i in "${!pids[@]}"; do
  if wait "${pids[$i]}"; then
    echo "[${names[$i]}] PASS"
  else
    echo "[${names[$i]}] FAIL (see $OUT/m00-${names[$i]}.log)"
    status=1
  fi
done

if [[ $status -eq 0 ]]; then
  echo "MATRIX: ALL 6 ROUTES PASS"
else
  echo "MATRIX: FAILURE"
fi
exit "$status"
