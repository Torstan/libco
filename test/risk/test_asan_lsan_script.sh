#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
fixture="$(mktemp -d "${TMPDIR:-/tmp}/libco-lsan-report.XXXXXX")"
trap 'rm -rf "$fixture"' EXIT
mkdir -p "$fixture/scripts/risk" "$fixture/test/risk/build-asan" "$fixture/bin"
cp "$ROOT/scripts/risk/run_asan_lsan.sh" "$fixture/scripts/risk/"

# Exercise the real report wrapper with controlled process output and exit codes.
# Building and running sanitizer binaries is covered by run_asan_lsan.sh itself.
cat >"$fixture/bin/make" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
cat >"$fixture/test/risk/build-asan/test_lifecycle_boundaries" <<'EOF'
#!/usr/bin/env bash
exit "${TEST_LIFECYCLE_EXIT:-0}"
EOF
cat >"$fixture/test/risk/build-asan/diag_leaks_and_boundaries" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$TEST_LEAK_OUTPUT"
exit "$TEST_LEAK_EXIT"
EOF
chmod +x "$fixture/bin/make" "$fixture/test/risk/build-asan/"*

failures=0
check_case() {
  local name="$1" output="$2" leak_exit="$3" expected_status="$4"
  local expected_exit="$5" lifecycle_exit="${6:-0}" actual_exit=0
  PATH="$fixture/bin:$PATH" TEST_LEAK_OUTPUT="$output" \
    TEST_LEAK_EXIT="$leak_exit" TEST_LIFECYCLE_EXIT="$lifecycle_exit" \
    bash "$fixture/scripts/risk/run_asan_lsan.sh" \
      >"$fixture/$name.log" 2>&1 || actual_exit=$?
  local actual_status
  actual_status="$(sed -n 's/^status: //p' "$fixture/logs/risk/P1-leaks.asan-lsan.log")"
  if [ "$actual_status" = "$expected_status" ] && [ "$actual_exit" -eq "$expected_exit" ]; then
    printf 'PASS: LSan report %s\n' "$name"
  else
    printf 'FAIL: %s expected status=%s exit=%s; got status=%s exit=%s\n' \
      "$name" "$expected_status" "$expected_exit" "$actual_status" "$actual_exit"
    failures=$((failures + 1))
  fi
}

check_case clean "env probe completed" 0 "not reproduced" 0
check_case leak "ERROR: LeakSanitizer: detected memory leaks" 1 "confirmed" 1
check_case unsupported \
  "==90432==AddressSanitizer: detect_leaks is not supported on this platform." \
  1 "needs environment" 0
check_case ptrace "LeakSanitizer does not work under ptrace" 1 "needs environment" 0
check_case fatal "LeakSanitizer has encountered a fatal error" 1 "needs environment" 0
check_case crashed "Segmentation fault" 139 "not run" 1
check_case asan_error "ERROR: AddressSanitizer: heap-use-after-free" 1 "not run" 1
check_case lifecycle_failed "env probe completed" 0 "not reproduced" 1 1

[ "$failures" -eq 0 ]
