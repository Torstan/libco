# Risk Verification Ledger

Date: 2026-05-08

Status values: `not run`, `confirmed`, `not reproduced`, `needs environment`,
`documented boundary`, `candidate regression`.

| Risk ID | Priority | Scenario | Command | Status | Evidence |
| --- | --- | --- | --- | --- | --- |
| P0-HOOK-FD-RACE | P0 | Global hook fd table race and UAF | `scripts/risk/run_tsan.sh` | needs environment | `logs/risk/P0-HOOK-FD-RACE.tsan.log`: hooked socket creation failed in sandbox; aggregate `run_tsan.sh` exits nonzero because `P1-COND-CROSS-THREAD` is confirmed |
| P0-HOOK-CLOSE-STALE | P0 | `close()` leaves stale hook metadata when hook is disabled | `make risk-check` | needs environment | `timeout 20s test/risk/build/test_hook_syscall_semantics`: socket failed with `EPERM`; child exited status 2 |
| P0-POLL-SAME-FD | P0 | Two coroutines poll the same fd | `make risk-check`; `scripts/risk/run_strace_poll.sh` | confirmed | `make risk-check` confirms semantic failure; `timeout 60s scripts/risk/run_strace_poll.sh` exits 0 with structured `needs environment` record when ptrace is denied |
| P0-HOOK-ALLOC-FD-LEAK | P0 | Hook metadata allocation failure leaks fd | `make risk-diagnose` | not reproduced | `timeout 30s make risk-diagnose`: `hooked_fd=-1 fd_count_before=102404 fd_count_after=102404` |
| P0-RESUME-ENDED | P0 | Resume ended coroutine | `make risk-check` | confirmed | `timeout 20s test/risk/build/test_lifecycle_boundaries`: child terminated by signal 11 |
| P1-POLL-ZERO-TIMEOUT | P1 | `co_poll(timeout=0)` semantics | `make risk-check` | confirmed | `make risk-check` |
| P1-POLL-FD-SEMANTICS | P1 | Invalid, closed, and regular fd polling semantics | `make risk-check` | confirmed | `make risk-check` |
| P1-CONNECT-ERRNO | P1 | Hooked `connect()` errno behavior | `make risk-check` | needs environment | `timeout 20s test/risk/build/test_hook_syscall_semantics`: bind local port failed with `EPERM`; child exited status 2 |
| P1-SETSOCKOPT-INVALID | P1 | Invalid timeout `setsockopt()` arguments | `make risk-check` | needs environment | `timeout 20s test/risk/build/test_hook_syscall_semantics`: socket creation failed with `EPERM`; child exited status 2 |
| P1-ENV-LEAK | P1 | Coroutine private environment leak | `scripts/risk/run_asan_lsan.sh` | needs environment | `timeout 60s scripts/risk/run_asan_lsan.sh` runs `diag_leaks_and_boundaries asan-leak-only`; `logs/risk/P1-leaks.asan-lsan.log`: wrapper appends structured `needs environment` after LeakSanitizer fatal |
| P1-THREADENV-LEAK | P1 | Per-thread `ThreadEnv` leak | `scripts/risk/run_asan_lsan.sh` | confirmed | `timeout 60s scripts/risk/run_asan_lsan.sh` runs `diag_leaks_and_boundaries asan-leak-only`; `logs/risk/P1-leaks.asan-lsan.log`: fd count grows after short-lived coroutine threads |
| P1-COND-CROSS-THREAD | P1 | `CoCond` cross-thread signal | `scripts/risk/run_tsan.sh` | confirmed | `timeout 60s scripts/risk/run_tsan.sh` exit 1; narrow `cond-only` run; `logs/risk/P1-COND-CROSS-THREAD.tsan.log`: TSan data race in `CoCond::Signal()`/`CoCond::Timedwait()` |
| P1-FUTURE-NO-CONTEXT | P1 | `Future::get()` outside coroutine context | `make risk-check` | confirmed | `timeout 20s test/risk/build/test_lifecycle_boundaries`: `co::Future<int>::wait()` assertion `thread_ctx` failed; child terminated by signal 6 |
| P1-PROMISE-ABANDONED | P1 | Abandoned promise behavior | `make risk-check` | confirmed | `timeout 20s test/risk/build/test_lifecycle_boundaries`: abandoned future `get()` inside coroutine reaches `co::Future<int>::schedule()` assertion `_promise`; child terminated by signal 6 |
| P1-COROUTINE-THROW | P1 | Exception crossing coroutine boundary | `make risk-check` | confirmed | `timeout 20s test/risk/build/test_lifecycle_boundaries`: uncaught `std::runtime_error("boom")`; child terminated by signal 6 |
| P1-ALLOC-FAILURE | P1 | Allocation failure safety | `make risk-diagnose` | confirmed | `timeout 30s make risk-diagnose`: constrained child terminated by signal 6 after `std::bad_alloc` |
| P2-IDLE-CPU | P2 | Event loop idle CPU | `make risk-diagnose` | not reproduced | `timeout 30s make risk-diagnose`: `cpu_us=1614 duration_ms=200` |
| P2-LONG-TIMEOUT | P2 | Timeout wheel long-timeout behavior | `make risk-diagnose` | needs environment | `timeout 30s make risk-diagnose`: manual long-duration run excluded from short diagnostic binary |
| P2-RUN-LOOP-CLEANUP | P2 | `run_loop(false)` final cleanup | `make risk-diagnose` | not reproduced | `timeout 30s make risk-diagnose`: `destructed_after_first=3 destructed_after_second=3` |
| P2-SCHEDULE-THREAD-LOCAL | P2 | `schedule()` thread-local boundary | `make risk-diagnose` | documented boundary | `timeout 30s make risk-diagnose`: cross-thread scheduled task did not run (`ran=0`) |
| P2-PLATFORM-GUARD | P2 | Platform and ABI guard | manual inspection | documented boundary | Manual inspection: `Makefile`, `CMakeLists.txt`, and `test/risk/Makefile` force `-m64`; no configure-time architecture guard beyond platform-specific linker flags |
