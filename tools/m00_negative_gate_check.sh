#!/bin/bash
# CLV2-M00.1 negative gate verification (run by the operator):
# forces the nano route to fail by feeding an invalid profile, then
# checks that m00_matrix.sh exits non-zero.  Then restores the good logs
# by re-running the real matrix once.  A trap removes the temporary
# bad script even if the run is interrupted.
set -u
cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 1

cleanup_bad() {
  rm -f tools/m00_matrix_bad.sh /tmp/m00_bad_run.log /tmp/m00_restore.log
}
trap cleanup_bad EXIT INT TERM

sed 's/-DUCN_PROFILE=NANO/-DUCN_PROFILE=BOGUS/' tools/m00_matrix.sh > tools/m00_matrix_bad.sh
bash tools/m00_matrix_bad.sh > /tmp/m00_bad_run.log 2>&1
rc=$?
if [ "$rc" -ne 0 ]; then
  echo "NEGATIVE-GATE: forced failure correctly exited non-zero (rc=$rc)"
else
  echo "NEGATIVE-GATE: BUG - forced failure exited zero"
  exit 1
fi
rm -f tools/m00_matrix_bad.sh /tmp/m00_bad_run.log

# restore the canonical logs
bash tools/m00_matrix.sh > /tmp/m00_restore.log 2>&1
rc=$?
rm -f /tmp/m00_restore.log
if [ "$rc" -eq 0 ]; then
  echo "NEGATIVE-GATE: canonical matrix restored (6/6 PASS)"
else
  echo "NEGATIVE-GATE: canonical matrix restore failed (rc=$rc)"
  exit 1
fi
exit 0
