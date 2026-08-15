#!/bin/bash
set -u
cd /mnt/e/File/MESH/UCN || exit 1
rm -rf build-m00-full build-m00-lite build-m00-nano build-m00-asan build-m00-analyzer
rm -f m00-*.log m00-summary.txt
run_one() {
  name="$1"; shift
  dir="build-m00-$name"
  {
    echo "==== [$name] configure ===="
    cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Debug "$@" || { echo "[$name] CONFIGURE FAIL"; return 1; }
    echo "==== [$name] build ===="
    cmake --build "$dir" --parallel || { echo "[$name] BUILD FAIL"; return 1; }
    echo "==== [$name] ctest ===="
    ctest --test-dir "$dir" --output-on-failure || { echo "[$name] CTEST FAIL"; return 1; }
    echo "[$name] ALL_OK"
  } > "m00-$name.log" 2>&1
  rc=$?
  echo "[$name] rc=$rc" >> m00-summary.txt
  return 0
}
run_one full -DUCN_PROFILE=FULL &
run_one lite -DUCN_PROFILE=LITE &
run_one nano -DUCN_PROFILE=NANO &
run_one asan -DUCN_PROFILE=FULL -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" &
run_one analyzer -DUCN_PROFILE=FULL -DCMAKE_C_FLAGS="-fanalyzer" &
wait
echo ALL_DONE
cat m00-summary.txt