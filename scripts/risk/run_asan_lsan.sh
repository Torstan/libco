#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LOG_DIR="$ROOT/logs/risk"
mkdir -p "$LOG_DIR"

BUILD_DIR="build-asan"
SANITIZER_PATTERNS="ERROR: AddressSanitizer|ERROR: LeakSanitizer|LeakSanitizer has encountered a fatal error|runtime error:|UndefinedBehaviorSanitizer|SUMMARY: UndefinedBehaviorSanitizer"

make -C "$ROOT/test/risk" clean BUILD_DIR="$BUILD_DIR"
make -C "$ROOT/test/risk" all \
  BUILD_DIR="$BUILD_DIR" SAN_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"

set +e
ASAN_OPTIONS="detect_leaks=1:abort_on_error=0" \
  "$ROOT/test/risk/$BUILD_DIR/test_lifecycle_boundaries" >"$LOG_DIR/P0-P1-lifecycle.asan.log" 2>&1
lifecycle_status=$?
ASAN_OPTIONS="detect_leaks=1:abort_on_error=0" \
  "$ROOT/test/risk/$BUILD_DIR/diag_leaks_and_boundaries" leak-only >"$LOG_DIR/P1-leaks.asan-lsan.log" 2>&1
leak_status=$?
set -e

echo "ASan lifecycle log: $LOG_DIR/P0-P1-lifecycle.asan.log"
echo "LSan leak log: $LOG_DIR/P1-leaks.asan-lsan.log"
echo "ASan lifecycle status: $lifecycle_status"
echo "LSan leak status: $leak_status"

if grep -E -q "$SANITIZER_PATTERNS" \
  "$LOG_DIR/P0-P1-lifecycle.asan.log" \
  "$LOG_DIR/P1-leaks.asan-lsan.log"; then
  exit 1
fi

if [ "$lifecycle_status" -ne 0 ] || [ "$leak_status" -ne 0 ]; then
  exit 1
fi
