#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LOG_DIR="$ROOT/logs/risk"
mkdir -p "$LOG_DIR"

BUILD_DIR="build-asan"
SANITIZER_PATTERNS="ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:|UndefinedBehaviorSanitizer|SUMMARY: UndefinedBehaviorSanitizer"
LSAN_ENVIRONMENT_PATTERNS="LeakSanitizer has encountered a fatal error|LeakSanitizer does not work under ptrace"

write_env_leak_record() {
  local status="$1"
  local actual="$2"
  cat <<EOF >>"$LOG_DIR/P1-leaks.asan-lsan.log"
RISK-ID: P1-ENV-LEAK
scenario: coroutine private environment leak
expected: LSan reports no per-coroutine env leak
actual: $actual
status: $status
regression: scripts/risk/run_asan_lsan.sh

EOF
}

make -C "$ROOT/test/risk" clean BUILD_DIR="$BUILD_DIR"
make -C "$ROOT/test/risk" all \
  BUILD_DIR="$BUILD_DIR" SAN_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"

set +e
ASAN_OPTIONS="detect_leaks=1:abort_on_error=0" \
  "$ROOT/test/risk/$BUILD_DIR/test_lifecycle_boundaries" >"$LOG_DIR/P0-P1-lifecycle.asan.log" 2>&1
lifecycle_status=$?
ASAN_OPTIONS="detect_leaks=1:abort_on_error=0" \
  "$ROOT/test/risk/$BUILD_DIR/diag_leaks_and_boundaries" asan-leak-only >"$LOG_DIR/P1-leaks.asan-lsan.log" 2>&1
leak_status=$?
set -e

echo "ASan lifecycle log: $LOG_DIR/P0-P1-lifecycle.asan.log"
echo "LSan leak log: $LOG_DIR/P1-leaks.asan-lsan.log"
echo "ASan lifecycle status: $lifecycle_status"
echo "LSan leak status: $leak_status"

lsan_environment=0
if grep -E -q "$LSAN_ENVIRONMENT_PATTERNS" "$LOG_DIR/P1-leaks.asan-lsan.log"; then
  lsan_environment=1
  write_env_leak_record "needs environment" \
    "LeakSanitizer could not complete in this environment"
elif grep -E -q "ERROR: LeakSanitizer" "$LOG_DIR/P1-leaks.asan-lsan.log"; then
  write_env_leak_record "confirmed" "LeakSanitizer reported a leak"
else
  write_env_leak_record "not reproduced" \
    "env probe completed without a LeakSanitizer leak report"
fi

if grep -E -q "$SANITIZER_PATTERNS" \
  "$LOG_DIR/P0-P1-lifecycle.asan.log" \
  "$LOG_DIR/P1-leaks.asan-lsan.log"; then
  exit 1
fi

if grep -E -q "^status: confirmed$" "$LOG_DIR/P1-leaks.asan-lsan.log"; then
  exit 1
fi

if [ "$lifecycle_status" -ne 0 ]; then
  exit 1
fi

if [ "$leak_status" -ne 0 ] && [ "$lsan_environment" -eq 0 ]; then
  exit 1
fi
