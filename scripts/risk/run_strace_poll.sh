#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LOG_DIR="$ROOT/logs/risk"
mkdir -p "$LOG_DIR"

make -C "$ROOT/test/risk" build/test_poll_semantics

write_needs_environment_record() {
  local actual="$1"
  cat <<EOF
RISK-ID: P0-POLL-SAME-FD
scenario: two coroutines poll the same fd
expected: strace can observe poll/epoll calls
actual: $actual
status: needs environment
regression: scripts/risk/run_strace_poll.sh

EOF
}

if ! command -v strace >/dev/null 2>&1; then
  write_needs_environment_record "strace is not installed" | tee "$LOG_DIR/P0-POLL-SAME-FD.strace.log"
  : >"$LOG_DIR/P0-POLL-SAME-FD.strace.stdout.log"
  exit 0
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

if [ "$status" -ne 0 ] &&
   grep -E -qi "operation not permitted|ptrace|PTRACE|permission denied" "$LOG_DIR/P0-POLL-SAME-FD.strace.log"; then
  write_needs_environment_record "strace failed because ptrace is not permitted in this environment" |
    tee -a "$LOG_DIR/P0-POLL-SAME-FD.strace.log"
  exit 0
fi

exit "$status"
