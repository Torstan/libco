#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LOG_DIR="$ROOT/logs/risk"
mkdir -p "$LOG_DIR"

make -C "$ROOT/test/risk" build/test_poll_semantics

if ! command -v strace >/dev/null 2>&1; then
  echo "strace is not installed"
  exit 2
fi

set +e
strace -f -e trace=epoll_ctl,epoll_wait,poll \
  "$ROOT/test/risk/build/test_poll_semantics" \
  >"$LOG_DIR/P0-POLL-SAME-FD.strace.stdout.log" \
  2>"$LOG_DIR/P0-POLL-SAME-FD.strace.log"
status=$?
set -e

echo "strace stdout log: $LOG_DIR/P0-POLL-SAME-FD.strace.stdout.log"
echo "strace log: $LOG_DIR/P0-POLL-SAME-FD.strace.log"
echo "strace status: $status"
exit "$status"
