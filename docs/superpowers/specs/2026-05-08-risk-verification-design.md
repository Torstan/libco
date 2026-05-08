# Libco Risk Verification Design

Date: 2026-05-08
Status: Approved for verification planning

## Purpose

This design defines how to verify the risks found in the current libco
implementation before deciding fixes. The goal is to produce evidence, update
risk severity, and select stable regressions. This phase does not change
production implementation.

## Scope

The verification covers all previously identified risks. Each risk must have at
least one verification path and a recorded status. Critical and high-risk issues
are verified first with executable evidence.

The approach is risk-driven and layered:

1. Confirm the highest-risk issues with small targeted programs.
2. Use sanitizer and system diagnostics for races, leaks, and syscall ordering.
3. Promote stable confirmed cases into automated regression targets.
4. Keep expensive, platform-specific, or scheduler-sensitive checks in a
   diagnostic suite.

## Non-Goals

- Do not fix the implementation in this phase.
- Do not make `make check` slower or less deterministic.
- Do not require every risk to become a default automated test.
- Do not modify production code for testability unless a later plan approves a
  minimal fault-injection seam for allocation failure cases.

## Verification Structure

Add a separate verification layer alongside the current tests:

- `test/risk/`: focused C++ verification programs. Each program validates one
  risk cluster and can be run independently.
- `scripts/risk/`: helper scripts for sanitizer builds, stress loops, fd
  counting, `strace` wrappers, and report aggregation.
- `docs/risk-verification.md`: verification ledger with status, evidence, and
  regression decisions.
- `Makefile` and `test/Makefile`: add non-default targets:
  - `risk-check`: stable, short risk regressions.
  - `risk-diagnose`: sanitizer, tracing, leak, and stress diagnostics.

Existing `make check` remains the baseline fast suite.

## Status Model

Each risk receives one status:

- `confirmed`: a deterministic failure, sanitizer report, syscall trace, leak
  report, fd-count growth, or oracle behavior mismatch proves the risk.
- `not reproduced`: the scenario ran without evidence. The code-review risk may
  remain open.
- `needs environment`: the local machine cannot verify it, for example
  BSD/macOS/arm64 behavior.
- `documented boundary`: the behavior exists but is accepted as unsupported API
  usage and must be documented or guarded.
- `candidate regression`: the issue is confirmed and stable enough for
  `risk-check` or `risk-diagnose`.

## Report Format

Each verification program or script should print enough structured information
to update the ledger:

```text
RISK-ID: P0-HOOK-FD-RACE
scenario: multi-thread hook fd table access
command: scripts/risk/run_tsan.sh test/risk/hook_fd_table_race
expected: no data race, no UAF, no stale metadata
actual: TSan reports race on g_rpchook_socket_fd
status: confirmed
regression: risk-diagnose only
evidence: logs/risk/P0-HOOK-FD-RACE.tsan.log
```

## Priority Matrix

### P0: Verify First

These risks need executable evidence before implementation planning.

- Global hook fd table race and UAF.
  - Verification: multi-thread hooked socket create, I/O, and close stress under
    TSan.
  - Confirmed by: TSan race on `g_rpchook_socket_fd`, UAF, crash, or stale fd
    metadata.
  - Regression target: `risk-diagnose`.

- `close()` leaves stale metadata when hook is disabled.
  - Verification: create hooked socket, set distinctive timeout or state, close
    in a non-hook context, then force fd reuse.
  - Confirmed by: new fd inherits old hook metadata or behavior.
  - Regression target: `risk-check` if stable.

- Two coroutines polling the same fd conflict.
  - Verification: two coroutines call `co_poll` on the same pipe or socket fd,
    then a writer makes it ready.
  - Confirmed by: one waiter timing out, lost wakeup, or `strace` showing failed
    `ADD` followed by unrelated `DEL`.
  - Regression target: `risk-check`; syscall trace remains diagnostic evidence.

- `socket()` and `co_accept()` leak fd when hook metadata allocation fails.
  - Verification: high-fd setup or controlled allocation failure to make
    `alloc_by_fd()` return null.
  - Confirmed by: function returns `-1` while `/proc/self/fd` count increases.
  - Regression target: `risk-diagnose` unless a stable fault-injection seam is
    later approved.

- Resuming an ended coroutine.
  - Verification: run in a child process, let coroutine finish, then call
    `co_resume` again under ASan/UBSan.
  - Confirmed by: signal, sanitizer error, invalid return, or assertion.
  - Regression target: `risk-check` with child-process isolation if stable.

### P1: Verify After P0 Evidence

These risks should have at least one runnable check or diagnostic.

- `co_poll(timeout=0)` semantics.
  - Compare system `poll` and `co_poll` on not-ready fds.
  - Confirmed by: `co_poll` yielding or failing to return immediately.

- Invalid, closed, and regular fd polling semantics.
  - Compare return value, errno, and `revents` with system `poll`.
  - Confirmed by: missing `POLLNVAL`, incorrect timeout, or oracle mismatch.

- `connect()` error errno.
  - Connect to a local unused TCP port.
  - Confirmed by: returning `ETIMEDOUT` or another wrong errno instead of the
    system `ECONNREFUSED` behavior.

- `setsockopt(SO_RCVTIMEO/SO_SNDTIMEO)` invalid argument handling.
  - Pass null and short `option_len` values.
  - Confirmed by: crash, ASan report, or hook timeout mutation despite syscall
    failure.

- Coroutine private environment leak.
  - Loop creating hooked-env coroutines and freeing them under LSan.
  - Confirmed by: per-coroutine env allocations reported as leaked.

- Per-thread `ThreadEnv` leak.
  - Loop creating and joining threads that initialize coroutine state.
  - Confirmed by: LSan leak report or persistent fd-count growth.

- `CoCond` cross-thread signal.
  - Wait in one thread and signal from another under TSan.
  - Confirmed by: data race, list corruption, wrong-thread resume, or hang.

- `Future::get()` outside coroutine context.
  - Call `get()` on a not-ready future without a coroutine/worker context.
  - Confirmed by: assert, abort, hang, or documented-boundary decision.

- Abandoned `Promise` behavior.
  - Destroy a promise without setting a result, then observe the future.
  - Confirmed by: debug assert, release hang, invalid state, or
    documented-boundary decision.

- Exception crossing coroutine boundary.
  - Throw directly from a coroutine body.
  - Confirmed by: terminate, crash, sanitizer report, or documented-boundary
    decision.

- Allocation failure safety.
  - Use resource limits or later-approved fault injection to fail stack or epoll
    result allocation.
  - Confirmed by: null dereference, crash, or sanitizer report.

### P2: Boundary and Performance Diagnostics

These are lower priority and may remain diagnostic-only.

- Event loop idle CPU.
  - Run an empty event loop for a fixed duration and compare CPU time with a
    blocking baseline.
  - Confirmed by: materially elevated idle CPU usage.

- Timeout wheel long-timeout behavior.
  - Register a timeout greater than 60 seconds and measure internal wakeups and
    final delay.
  - Confirmed by: excessive periodic re-add activity or large timing error.

- `run_loop(false)` final coroutine cleanup.
  - Use destructor counters around one-shot async tasks.
  - Confirmed by: cleanup only after a second `run_loop(false)` call.

- `schedule()` thread-local boundary.
  - Schedule from one thread and run a worker in another.
  - Confirmed by: task does not execute and API docs do not describe this
    boundary.

- Platform and ABI guard.
  - On current Linux x86_64, inspect build configuration for explicit platform
    constraints. Non-x86/macOS/BSD runtime checks are marked `needs environment`.
  - Confirmed by: unsupported platforms lacking configure-time failure or
    documented limitation.

## Execution Order

1. Run `make check` as baseline.
2. Run stable P0 tests through `risk-check`.
3. Run P0 diagnostics through TSan, ASan/UBSan/LSan, `strace`, and fd counting.
4. Run P1 checks grouped by semantics, lifecycle, leaks, and API boundaries.
5. Run P2 performance and portability diagnostics when time and environment
   allow.
6. Update `docs/risk-verification.md`.
7. Promote stable confirmed P0/P1 checks to `candidate regression`.

## Tooling

- TSan: hook fd table races, `CoCond` cross-thread usage, cross-thread
  scheduling boundaries.
- ASan/UBSan/LSan: ended coroutine resume, allocation failure, env leaks,
  `ThreadEnv` leaks, exception boundary behavior.
- `strace`: `epoll_ctl` ADD/DEL ordering, connect error paths, syscall oracle
  comparisons.
- `/proc/self/fd` or `lsof`: fd leaks and per-thread epoll fd growth.
- System-call oracle programs: compare libco hooks with native `poll`,
  `connect`, and `setsockopt` behavior.

## Automation Policy

`risk-check` should contain stable, short, deterministic checks. It is allowed
to fail on current HEAD while risks are being confirmed, but failures must be
clear and attributable.

`risk-diagnose` may be slower, sanitizer-dependent, timing-sensitive, or
platform-sensitive. It should write logs and summarize evidence rather than
requiring every run to reproduce races.

Crash-oriented checks must run in child processes so one confirmed crash does
not abort the whole suite.

## Success Criteria

- Every reviewed risk has a verification path and a ledger status.
- P0 risks have runtime evidence, not only code-review reasoning.
- Stable confirmed P0/P1 cases are selected for regression coverage.
- Expensive or scheduler-sensitive findings remain reproducible through
  `risk-diagnose`.
- The resulting evidence is sufficient to prioritize a later fix plan.
