#!/bin/sh
# Phase 1 checkpoint: write a burst, kill -9 the process mid-write, restart,
# confirm recovery (no crash, no corruption, a plausible partial point count).
set -e

TOOL=./build/strata_tool
DATA_DIR=/tmp/strata_phase1_crash_test
NUM_POINTS=3000000
KILL_AFTER=1  # seconds

rm -rf "$DATA_DIR"

echo "=== starting burst write of $NUM_POINTS points ==="
"$TOOL" write "$DATA_DIR" "$NUM_POINTS" > /tmp/strata_phase1_write.log 2>&1 &
PID=$!

sleep "$KILL_AFTER"

if kill -0 "$PID" 2>/dev/null; then
  echo "=== killing PID $PID with SIGKILL mid-write ==="
  kill -9 "$PID"
  wait "$PID" 2>/dev/null || true
  KILLED=1
else
  echo "=== write finished before the kill window elapsed ==="
  KILLED=0
fi

tail -3 /tmp/strata_phase1_write.log

echo "=== restarting and recovering ==="
"$TOOL" recover "$DATA_DIR" | tee /tmp/strata_phase1_recover.log

TOTAL=$(grep total_points /tmp/strata_phase1_recover.log | sed 's/.*=//')

if [ "$KILLED" = "1" ]; then
  if [ "$TOTAL" -gt 0 ] && [ "$TOTAL" -lt "$NUM_POINTS" ]; then
    echo "PASS: recovered $TOTAL/$NUM_POINTS points after SIGKILL, no crash on restart"
  else
    echo "FAIL: recovered point count ($TOTAL) is not a plausible partial result"
    exit 1
  fi
else
  if [ "$TOTAL" -eq "$NUM_POINTS" ]; then
    echo "PASS (write completed before kill): all $TOTAL points present"
  else
    echo "FAIL: expected $NUM_POINTS points, got $TOTAL"
    exit 1
  fi
fi
