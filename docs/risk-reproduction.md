# Risk Reproduction Guide

Date: 2026-05-08

This document explains how to reproduce the risks recorded in
`docs/risk-verification.md`. The reproduction suite is opt-in: the default
`make check` path does not run these probes.

## Prerequisites

- Linux x86_64 build environment with `make`, `g++`, pthread, and dl.
- Optional diagnostics:
  - `strace` and ptrace permission for syscall tracing.
  - GCC/Clang sanitizer runtime for TSan, ASan, UBSan, and LSan.
  - Socket and bind permission for hook syscall probes.

Some sandboxed environments deny socket creation, bind, ptrace, or LSan
operation. In that case the suite prints `status: needs environment` instead
of treating the missing capability as proof of the target risk.

## Quick Reproduction

Run the baseline tests first:

```bash
timeout 60s make check
```

Expected result: exit `0`.

Run stable risk checks:

```bash
timeout 30s make risk-check
```

Expected result: nonzero when non-poll risks are confirmed. On the current
branch, all poll records are expected to report `not reproduced`.

The current branch fixed these poll risks:

- `P0-POLL-SAME-FD`: fixed by `5e5e569` and `ca5d032`.
- `P1-POLL-ZERO-TIMEOUT`: fixed by `4e44aa2` and `32d93ec`.
- `P1-POLL-FD-SEMANTICS`: fixed by `4e44aa2` and `32d93ec`.

The same command still reproduces these lifecycle risks:

- `P0-RESUME-ENDED`: resuming an ended coroutine terminates the child process.
- `P1-FUTURE-NO-CONTEXT`: `Future::get()` outside coroutine context aborts.
- `P1-PROMISE-ABANDONED`: abandoned promise aborts instead of reporting a
  bounded broken-promise result.
- `P1-COROUTINE-THROW`: exception crossing coroutine boundary terminates the
  process.

Hook syscall checks may print `needs environment` if the host denies socket or
bind operations.

Run diagnostic checks:

```bash
timeout 30s make risk-diagnose
```

Expected result: nonzero when diagnostic risks are confirmed. On the current
sandbox this reproduces:

- `P1-THREADENV-LEAK`: fd count grows after short-lived coroutine threads.
- `P1-ALLOC-FAILURE`: constrained allocation failure terminates the child
  process instead of returning a controlled error.
- `P1-COND-CROSS-THREAD`: cross-thread `CoCond::Signal()` is unsafe and may
  crash; TSan gives stronger evidence via the TSan wrapper.

The same command also records non-reproduced or documented-boundary results for
P2 probes.

## Focused Binaries

The top-level targets build the focused binaries under `test/risk/build`.
After running `make risk-check` or `make risk-diagnose`, rerun a single binary
when narrowing one class of issue:

```bash
timeout 20s test/risk/build/test_poll_semantics
timeout 20s test/risk/build/test_lifecycle_boundaries
timeout 20s test/risk/build/test_hook_syscall_semantics
timeout 20s test/risk/build/diag_leaks_and_boundaries
timeout 20s test/risk/build/diag_perf_boundaries
```

Useful narrowed modes:

```bash
timeout 10s test/risk/build/diag_leaks_and_boundaries leak-only
timeout 10s test/risk/build/diag_leaks_and_boundaries cond-only
```

The `leak-only` mode runs only coroutine env and `ThreadEnv` probes. The
`cond-only` mode isolates the cross-thread `CoCond` probe for TSan.

## Sanitizer and Trace Reproduction

Run TSan diagnostics:

```bash
timeout 60s scripts/risk/run_tsan.sh
```

Logs:

- `logs/risk/P0-HOOK-FD-RACE.tsan.log`
- `logs/risk/P1-COND-CROSS-THREAD.tsan.log`

Expected result: nonzero when TSan reports or a diagnostic risk is confirmed.
In this sandbox, the hook fd race probe is environment-limited because hooked
socket creation fails, while the `CoCond` diagnostic is confirmed.

Run ASan/UBSan/LSan diagnostics:

```bash
timeout 60s scripts/risk/run_asan_lsan.sh
```

Logs:

- `logs/risk/P0-P1-lifecycle.asan.log`
- `logs/risk/P1-leaks.asan-lsan.log`

Expected result: nonzero when lifecycle sanitizer findings, confirmed
ThreadEnv growth, or sanitizer failures are present. The wrapper appends a
structured `P1-ENV-LEAK` record. If LSan cannot complete in the environment,
that record is `status: needs environment`.

Run strace poll diagnostics:

```bash
timeout 60s scripts/risk/run_strace_poll.sh
```

Logs:

- `logs/risk/P0-POLL-SAME-FD.strace.log`
- `logs/risk/P0-POLL-SAME-FD.strace.stdout.log`

Expected result: if ptrace is denied, the script exits `0` and appends a
structured `needs environment` record. If strace can run, nonzero status should
be treated as a real traced-program result rather than a ptrace setup failure.

## Reading Results

Each probe prints records in this format:

```text
RISK-ID: <id>
scenario: <what is being exercised>
expected: <safe or reference behavior>
actual: <observed behavior>
status: <classification>
regression: <target or script>
```

Interpret statuses as follows:

- `confirmed`: the bug or risk reproduced.
- `not reproduced`: the probe ran and did not observe the risk.
- `needs environment`: the host blocked a prerequisite, so the probe could not
  prove or disprove the risk.
- `documented boundary`: behavior is observable but currently treated as an API
  boundary rather than a failing regression.

`make risk-check` and `make risk-diagnose` intentionally return nonzero when
any probe reports `confirmed`. A nonzero exit from these targets is therefore
evidence to inspect the structured records, not by itself a harness failure.

## Current Sandbox Notes

The current environment has these limitations:

- Hook syscall probes hit `EPERM` for socket or bind operations.
- `strace` cannot attach because ptrace is denied.
- LSan can emit `LeakSanitizer has encountered a fatal error`, so
  `P1-ENV-LEAK` remains `needs environment`.

The confirmed poll, lifecycle, ThreadEnv, allocation, and `CoCond` issues are
still reproducible without those missing capabilities.
