#!/bin/sh
# Phase 6 checkpoint: formal crash-recovery testing at specific, named
# points -- not a timed external kill (tests/phase1_crash_recovery.sh),
# but deterministic fault injection (see src/strata/fault_injection.hpp)
# so each of these scenarios reliably lands exactly where it claims to.
#
# Each scenario: run `crashtest <point>` with STRATA_CRASH_AT=<point> set,
# confirm the process actually died by SIGKILL (exit 137), then run
# `recover` and check the exact expected outcome -- including the two
# scenarios that are *supposed* to reproduce already-documented gaps
# (ARCHITECTURE.md), formally confirming they're bounded exactly as
# claimed rather than just asserting it in prose.
#
# Deliberately no `set -e`: a killed child's exit status is itself the
# thing being checked, and later scenarios should still run and report
# even if an earlier one doesn't match expectations.

TOOL=./build/strata_tool
FAIL=0

field() {  # field <name> <recover output>
  echo "$2" | grep "^recover: $1=" | sed 's/.*=//'
}

run_point() {
  point="$1"
  data_dir="/tmp/strata_phase6_$point"
  rm -rf "$data_dir"

  echo "=== $point ==="
  STRATA_CRASH_AT="$point" "$TOOL" crashtest "$point" "$data_dir" > /tmp/strata_phase6_crash.log 2>&1
  status=$?
  if [ "$status" -ne 137 ]; then
    echo "FAIL: expected SIGKILL (exit 137), got exit $status"
    cat /tmp/strata_phase6_crash.log
    FAIL=1
    return
  fi
  echo "  killed as expected (exit 137)"

  RECOVER_OUT=$("$TOOL" recover "$data_dir")
  echo "$RECOVER_OUT" | sed 's/^/  /'
}

# --- 1. mid-MemTable-buildup: no flush ever happened. Every write must
#     come back purely via WAL replay. ---
run_point mid_memtable_buildup
if [ "$(field points_replayed_from_wal "$RECOVER_OUT")" = "500" ] && \
   [ "$(field l0_blocks "$RECOVER_OUT")" = "0" ] && \
   [ "$(field l1_blocks "$RECOVER_OUT")" = "0" ] && \
   [ "$(field total_points "$RECOVER_OUT")" = "500" ]; then
  echo "  PASS: all 500 points recovered from WAL alone, no blocks"
else
  echo "  FAIL: expected 500 points via WAL replay, 0 blocks"
  FAIL=1
fi

# --- 2. mid-flush, before MANIFEST is updated: the new L0 block is a
#     valid-but-unlisted orphan. Cleanup must delete it; WAL (untouched)
#     must still recover everything. No loss. ---
run_point post_l0_write_pre_manifest
if [ "$(field l0_blocks "$RECOVER_OUT")" = "0" ] && \
   [ "$(field points_replayed_from_wal "$RECOVER_OUT")" = "100" ] && \
   [ "$(field total_points "$RECOVER_OUT")" = "100" ]; then
  echo "  PASS: orphaned L0 block cleaned up, all 100 points recovered via WAL"
else
  echo "  FAIL: expected orphan cleanup + exactly 100 points via WAL"
  FAIL=1
fi

# --- 3. mid-flush, after MANIFEST is updated but before the WAL is
#     unlinked: this IS the documented flush/WAL-reset gap. The new L0
#     block is live AND the WAL still has the same records -> replay
#     double-counts. No corruption, no *loss* -- just the known,
#     bounded duplication, confirmed here rather than assumed. ---
run_point post_manifest_pre_wal_unlink
if [ "$(field l0_blocks "$RECOVER_OUT")" = "1" ] && \
   [ "$(field l0_points "$RECOVER_OUT")" = "100" ] && \
   [ "$(field points_replayed_from_wal "$RECOVER_OUT")" = "100" ] && \
   [ "$(field total_points "$RECOVER_OUT")" = "200" ]; then
  echo "  PASS: confirmed known gap -- 100 points duplicated to 200 (100 in L0 + 100 replayed), not lost or corrupted"
else
  echo "  FAIL: expected the documented duplicate-points outcome (200 total)"
  FAIL=1
fi

# --- 4. mid-compaction, after the new L1 block is written but before
#     MANIFEST is swapped: old L0 blocks are still manifest-live and
#     untouched, so no data was ever at risk. The orphaned L1 file gets
#     cleaned up; original data survives entirely via L0. ---
run_point post_l1_write_pre_manifest_rename
if [ "$(field l0_blocks "$RECOVER_OUT")" = "1" ] && \
   [ "$(field l0_points "$RECOVER_OUT")" = "50" ] && \
   [ "$(field l1_blocks "$RECOVER_OUT")" = "0" ]; then
  echo "  PASS: orphaned L1 block cleaned up, original L0 data untouched"
  "$TOOL" compact "/tmp/strata_phase6_post_l1_write_pre_manifest_rename" 60000 0 > /tmp/strata_phase6_retry.log 2>&1
  RETRY_OUT=$("$TOOL" recover "/tmp/strata_phase6_post_l1_write_pre_manifest_rename")
  if [ "$(field l1_blocks "$RETRY_OUT")" = "1" ] && [ "$(field l0_blocks "$RETRY_OUT")" = "0" ]; then
    echo "  PASS: compaction redone successfully on next run, as STRATA_DESIGN.md's MANIFEST section describes"
  else
    echo "  FAIL: retrying compaction after the crash did not succeed cleanly"
    FAIL=1
  fi
else
  echo "  FAIL: expected original L0 block intact (1 block, 50 points), orphan L1 cleaned up"
  FAIL=1
fi

# --- 5. mid-compaction, after MANIFEST is swapped but before the old L0
#     blocks are deleted: MANIFEST already correctly points at the new
#     L1 block. Cleanup must finish deleting the now-orphaned L0 files. ---
run_point post_manifest_rename_pre_l0_delete
if [ "$(field l0_blocks "$RECOVER_OUT")" = "0" ] && \
   [ "$(field l1_blocks "$RECOVER_OUT")" = "1" ] && \
   [ "$(field l1_buckets "$RECOVER_OUT")" -gt "0" ]; then
  echo "  PASS: superseded L0 block cleaned up, data present via the new L1 block"
else
  echo "  FAIL: expected L0 cleaned up (0 blocks), L1 already live with buckets"
  FAIL=1
fi

echo ""
rm -rf /tmp/strata_phase6_* 2>/dev/null || true
if [ "$FAIL" -eq 0 ]; then
  echo "PASS: all Phase 6 crash-recovery scenarios behaved exactly as documented"
  exit 0
else
  echo "FAIL: one or more Phase 6 scenarios did not match expected recovery behavior"
  exit 1
fi
