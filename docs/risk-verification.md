# Risk Verification Ledger

Date: 2026-05-08

Status values: `not run`, `confirmed`, `not reproduced`, `needs environment`,
`documented boundary`, `candidate regression`.

| Risk ID | Priority | Scenario | Command | Status | Evidence |
| --- | --- | --- | --- | --- | --- |
| P0-HOOK-FD-RACE | P0 | Global hook fd table race and UAF | `scripts/risk/run_tsan.sh` | not run | |
| P0-HOOK-CLOSE-STALE | P0 | `close()` leaves stale hook metadata when hook is disabled | `make risk-check` | needs environment | `timeout 20s test/risk/build/test_hook_syscall_semantics`: socket failed with `EPERM`; child exited status 2 |
| P0-POLL-SAME-FD | P0 | Two coroutines poll the same fd | `make risk-check` | confirmed | `make risk-check` |
| P0-HOOK-ALLOC-FD-LEAK | P0 | Hook metadata allocation failure leaks fd | `make risk-diagnose` | not run | |
| P0-RESUME-ENDED | P0 | Resume ended coroutine | `make risk-check` | not run | |
| P1-POLL-ZERO-TIMEOUT | P1 | `co_poll(timeout=0)` semantics | `make risk-check` | confirmed | `make risk-check` |
| P1-POLL-FD-SEMANTICS | P1 | Invalid, closed, and regular fd polling semantics | `make risk-check` | confirmed | `make risk-check` |
| P1-CONNECT-ERRNO | P1 | Hooked `connect()` errno behavior | `make risk-check` | needs environment | `timeout 20s test/risk/build/test_hook_syscall_semantics`: bind unused port failed with `EPERM`; child exited status 2 |
| P1-SETSOCKOPT-INVALID | P1 | Invalid timeout `setsockopt()` arguments | `make risk-check` | needs environment | `timeout 20s test/risk/build/test_hook_syscall_semantics`: socket creation failed with `EPERM`; child exited status 2 |
| P1-ENV-LEAK | P1 | Coroutine private environment leak | `scripts/risk/run_asan_lsan.sh` | not run | |
| P1-THREADENV-LEAK | P1 | Per-thread `ThreadEnv` leak | `scripts/risk/run_asan_lsan.sh` | not run | |
| P1-COND-CROSS-THREAD | P1 | `CoCond` cross-thread signal | `scripts/risk/run_tsan.sh` | not run | |
| P1-FUTURE-NO-CONTEXT | P1 | `Future::get()` outside coroutine context | `make risk-check` | not run | |
| P1-PROMISE-ABANDONED | P1 | Abandoned promise behavior | `make risk-check` | not run | |
| P1-COROUTINE-THROW | P1 | Exception crossing coroutine boundary | `make risk-check` | not run | |
| P1-ALLOC-FAILURE | P1 | Allocation failure safety | `make risk-diagnose` | not run | |
| P2-IDLE-CPU | P2 | Event loop idle CPU | `make risk-diagnose` | not run | |
| P2-LONG-TIMEOUT | P2 | Timeout wheel long-timeout behavior | `make risk-diagnose` | not run | |
| P2-RUN-LOOP-CLEANUP | P2 | `run_loop(false)` final cleanup | `make risk-diagnose` | not run | |
| P2-SCHEDULE-THREAD-LOCAL | P2 | `schedule()` thread-local boundary | `make risk-diagnose` | not run | |
| P2-PLATFORM-GUARD | P2 | Platform and ABI guard | manual inspection | not run | |
