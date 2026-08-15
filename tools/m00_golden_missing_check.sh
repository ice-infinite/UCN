#!/bin/bash
# CLV2-M00.1 negative golden check: a MISSING committed golden must
# make ucn_tests exit non-zero (fail closed).
set -u
cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 1

mv tests/golden/cluster_golden_trace.txt /tmp/golden_check_backup.txt
./build-m00-full/ucn_tests > /tmp/golden_missing_run.log 2>&1
rc=$?
mv /tmp/golden_check_backup.txt tests/golden/cluster_golden_trace.txt
if [ "$rc" -ne 0 ]; then
  echo "GOLDEN-MISSING-GATE: correctly failed (rc=$rc)"
  grep -o 'golden trace MISSING.*' /tmp/golden_missing_run.log | head -1
else
  echo "GOLDEN-MISSING-GATE: BUG - missing golden passed (rc=0)"
  rm -f /tmp/golden_missing_run.log
  exit 1
fi
# confirm the gate is green again after restore
./build-m00-full/ucn_tests > /tmp/golden_restore_run.log 2>&1
rc=$?
rm -f /tmp/golden_missing_run.log /tmp/golden_restore_run.log
if [ "$rc" -eq 0 ]; then
  echo "GOLDEN-MISSING-GATE: restored, normal run passes"
else
  echo "GOLDEN-MISSING-GATE: restore run failed (rc=$rc)"
  exit 1
fi
exit 0
