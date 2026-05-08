#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LOG_DIR="$ROOT/logs/risk"
mkdir -p "$LOG_DIR"

make -C "$ROOT/test/risk" clean
make -C "$ROOT/test/risk" build/diag_hook_fd_race build/diag_leaks_and_boundaries \
  SAN_FLAGS="-fsanitize=thread -fno-omit-frame-pointer"

set +e
"$ROOT/test/risk/build/diag_hook_fd_race" >"$LOG_DIR/P0-HOOK-FD-RACE.tsan.log" 2>&1
race_status=$?
"$ROOT/test/risk/build/diag_leaks_and_boundaries" >"$LOG_DIR/P1-COND-CROSS-THREAD.tsan.log" 2>&1
cond_status=$?
set -e

echo "TSan hook fd race log: $LOG_DIR/P0-HOOK-FD-RACE.tsan.log"
echo "TSan cond boundary log: $LOG_DIR/P1-COND-CROSS-THREAD.tsan.log"
echo "TSan hook fd race status: $race_status"
echo "TSan cond boundary status: $cond_status"

if grep -q "WARNING: ThreadSanitizer" "$LOG_DIR/P0-HOOK-FD-RACE.tsan.log" ||
   grep -q "WARNING: ThreadSanitizer" "$LOG_DIR/P1-COND-CROSS-THREAD.tsan.log"; then
  exit 1
fi

if [ "$race_status" -ne 0 ] || [ "$cond_status" -ne 0 ]; then
  exit 1
fi
