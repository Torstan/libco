# Risk Verification Suite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a separate risk verification suite that confirms libco implementation risks with targeted tests, diagnostics, and a verification ledger.

**Architecture:** Add a non-default `test/risk` test layer with focused C++ programs and small shell wrappers. Stable checks run under `make risk-check`; sanitizer, trace, leak, and stress checks run under `make risk-diagnose`. The suite records evidence in `docs/risk-verification.md` without changing production code or slowing `make check`.

**Tech Stack:** C++17, GNU Make, pthread, dl, POSIX sockets/poll/fcntl/fork, `/proc/self/fd`, optional TSan/ASan/UBSan/LSan and `strace`.

---

## File Structure

- Create `test/risk/risk_common.h`: shared result reporting, child-process isolation, fd counting, and small assertion helpers.
- Create `test/risk/Makefile`: builds risk programs by compiling libco sources directly so sanitizer flags apply to both tests and library code.
- Modify `Makefile`: add top-level `risk-check` and `risk-diagnose` targets.
- Modify `test/Makefile`: delegate risk targets into `test/risk`.
- Create `docs/risk-verification.md`: initial ledger with all risk IDs, commands, and blank evidence fields represented as `not run`.
- Create `test/risk/test_poll_semantics.cpp`: stable checks for same-fd waiters, zero-timeout `co_poll`, invalid fd, and regular fd semantics.
- Create `test/risk/test_hook_syscall_semantics.cpp`: stable checks for stale hook fd metadata, invalid `setsockopt`, and `connect` errno behavior.
- Create `test/risk/test_lifecycle_boundaries.cpp`: child-isolated checks for ended coroutine resume, future wait outside coroutine context, abandoned promise, and exceptions crossing coroutine boundaries.
- Create `test/risk/diag_hook_fd_race.cpp`: TSan-oriented hook fd table race diagnostic.
- Create `test/risk/diag_leaks_and_boundaries.cpp`: LSan/fd-count diagnostics for coroutine env leaks, `ThreadEnv` leaks, `CoCond` cross-thread signal, and thread-local scheduling boundaries.
- Create `test/risk/diag_perf_boundaries.cpp`: idle event loop CPU and long timeout boundary diagnostics.
- Create `scripts/risk/run_tsan.sh`, `scripts/risk/run_asan_lsan.sh`, and `scripts/risk/run_strace_poll.sh`: repeatable diagnostic commands that write logs under `logs/risk`.

## Task 1: Add Risk Harness, Build Targets, and Ledger

**Files:**
- Create: `test/risk/risk_common.h`
- Create: `test/risk/Makefile`
- Modify: `Makefile`
- Modify: `test/Makefile`
- Create: `docs/risk-verification.md`

- [ ] **Step 1: Create the risk helper header**

Create `test/risk/risk_common.h` with this full content:

```cpp
#pragma once

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <functional>
#include <string>
#include <vector>

namespace risk {

enum class Status {
  kPassed,
  kConfirmed,
  kNotReproduced,
  kNeedsEnvironment,
  kDocumentedBoundary,
};

struct Result {
  const char *id;
  const char *scenario;
  const char *expected;
  std::string actual;
  Status status;
  const char *regression;
};

inline const char *status_name(Status status) {
  switch (status) {
  case Status::kPassed:
    return "passed";
  case Status::kConfirmed:
    return "confirmed";
  case Status::kNotReproduced:
    return "not reproduced";
  case Status::kNeedsEnvironment:
    return "needs environment";
  case Status::kDocumentedBoundary:
    return "documented boundary";
  }
  return "unknown";
}

inline Result passed(const char *id, const char *scenario,
                     const char *expected, const std::string &actual,
                     const char *regression) {
  return Result{id, scenario, expected, actual, Status::kPassed, regression};
}

inline Result confirmed(const char *id, const char *scenario,
                        const char *expected, const std::string &actual,
                        const char *regression) {
  return Result{id, scenario, expected, actual, Status::kConfirmed,
                regression};
}

inline Result not_reproduced(const char *id, const char *scenario,
                             const char *expected, const std::string &actual,
                             const char *regression) {
  return Result{id, scenario, expected, actual, Status::kNotReproduced,
                regression};
}

inline Result needs_environment(const char *id, const char *scenario,
                                const char *expected,
                                const std::string &actual,
                                const char *regression) {
  return Result{id, scenario, expected, actual, Status::kNeedsEnvironment,
                regression};
}

inline Result documented_boundary(const char *id, const char *scenario,
                                  const char *expected,
                                  const std::string &actual,
                                  const char *regression) {
  return Result{id, scenario, expected, actual, Status::kDocumentedBoundary,
                regression};
}

inline void print_result(const Result &result) {
  printf("RISK-ID: %s\n", result.id);
  printf("scenario: %s\n", result.scenario);
  printf("expected: %s\n", result.expected);
  printf("actual: %s\n", result.actual.c_str());
  printf("status: %s\n", status_name(result.status));
  printf("regression: %s\n\n", result.regression);
}

inline int summarize(const std::vector<Result> &results) {
  int confirmed_count = 0;
  for (const Result &result : results) {
    print_result(result);
    if (result.status == Status::kConfirmed) {
      ++confirmed_count;
    }
  }
  return confirmed_count == 0 ? 0 : 1;
}

inline int count_open_fds() {
  DIR *dir = opendir("/proc/self/fd");
  if (!dir) {
    return -1;
  }
  int count = 0;
  while (readdir(dir)) {
    ++count;
  }
  closedir(dir);
  return count;
}

inline std::string child_status_text(int status) {
  char buf[128];
  if (WIFEXITED(status)) {
    snprintf(buf, sizeof(buf), "child exited with status %d",
             WEXITSTATUS(status));
  } else if (WIFSIGNALED(status)) {
    snprintf(buf, sizeof(buf), "child terminated by signal %d",
             WTERMSIG(status));
  } else {
    snprintf(buf, sizeof(buf), "child ended with raw status %d", status);
  }
  return std::string(buf);
}

inline int run_child(const std::function<void()> &fn) {
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return 255;
  }
  if (pid == 0) {
    fn();
    _exit(0);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      perror("waitpid");
      return 255;
    }
  }
  return status;
}

inline bool child_exited_cleanly(int status) {
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

inline void require_syscall(bool condition, const char *message) {
  if (!condition) {
    perror(message);
    _exit(2);
  }
}

inline void set_cpu_seconds_limit(int seconds) {
  struct rlimit limit;
  limit.rlim_cur = seconds;
  limit.rlim_max = seconds;
  setrlimit(RLIMIT_CPU, &limit);
}

} // namespace risk
```

- [ ] **Step 2: Create the risk Makefile**

Create `test/risk/Makefile` with this full content:

```make
ROOT := ../..
BUILD_DIR := build
CXX ?= g++

LIBCO_SRCS := \
	$(ROOT)/co_epoll.cpp \
	$(ROOT)/co_cond.cpp \
	$(ROOT)/thread_worker.cpp \
	$(ROOT)/routine_context.cpp \
	$(ROOT)/co_routine.cpp \
	$(ROOT)/co_hook_sys_call.cpp \
	$(ROOT)/coctx.cpp \
	$(ROOT)/coctx_swap.S

COMMON_FLAGS := -I$(ROOT) -I. -g -fno-strict-aliasing -O1 --std=c++17 \
	-Wall -Werror -pipe -D_GNU_SOURCE -D_REENTRANT -fPIC -Wno-deprecated -m64
COMMON_LIBS := -lpthread -ldl

RISK_CHECK_PROGS :=
RISK_DIAGNOSE_PROGS :=
ALL_PROGS := $(RISK_CHECK_PROGS) $(RISK_DIAGNOSE_PROGS)

.PHONY: all check diagnose clean

all: $(addprefix $(BUILD_DIR)/,$(ALL_PROGS))

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

define RISK_PROGRAM_template
$(BUILD_DIR)/$(1): $(1).cpp risk_common.h $(LIBCO_SRCS) | $(BUILD_DIR)
	$(CXX) $(COMMON_FLAGS) $(SAN_FLAGS) -o $$@ $(1).cpp $(LIBCO_SRCS) $(COMMON_LIBS) $(SAN_FLAGS)
endef

$(foreach prog,$(ALL_PROGS),$(eval $(call RISK_PROGRAM_template,$(prog))))

check: $(addprefix $(BUILD_DIR)/,$(RISK_CHECK_PROGS))
	@if [ -z "$(RISK_CHECK_PROGS)" ]; then \
		echo "No risk-check programs configured"; \
	else \
		status=0; \
		for prog in $(RISK_CHECK_PROGS); do \
			echo "RUN risk-check $$prog"; \
			$(BUILD_DIR)/$$prog || status=1; \
		done; \
		exit $$status; \
	fi

diagnose: $(addprefix $(BUILD_DIR)/,$(RISK_DIAGNOSE_PROGS))
	@if [ -z "$(RISK_DIAGNOSE_PROGS)" ]; then \
		echo "No risk-diagnose programs configured"; \
	else \
		status=0; \
		for prog in $(RISK_DIAGNOSE_PROGS); do \
			echo "RUN risk-diagnose $$prog"; \
			$(BUILD_DIR)/$$prog || status=1; \
		done; \
		exit $$status; \
	fi

clean:
	rm -rf $(BUILD_DIR)
```

- [ ] **Step 3: Add top-level risk targets**

Modify the root `Makefile` so the `.PHONY` line and target section become:

```make
.PHONY: all colib examples tests check risk-check risk-diagnose clean dist

examples: libcolib.a
	$(MAKE) -C example
tests: libcolib.a
	$(MAKE) -C test

check: tests
	$(MAKE) -C test check

risk-check:
	$(MAKE) -C test risk-check

risk-diagnose:
	$(MAKE) -C test risk-diagnose
```

- [ ] **Step 4: Add test-level risk targets**

Modify `test/Makefile` so the `.PHONY` line and target section become:

```make
.PHONY: all check risk-check risk-diagnose clean dist

test_co_routine:test_co_routine.o
	$(BUILDEXE)

test_co_async:test_co_async.o
	$(BUILDEXE)

test_co_poll:test_co_poll.o
	$(BUILDEXE)

test_public_api:test_public_api.o
	$(BUILDEXE)

check: all
	@set -e; for prog in $(PROGS); do echo "RUN $$prog"; ./$$prog; done

risk-check:
	$(MAKE) -C risk check

risk-diagnose:
	$(MAKE) -C risk diagnose
```

Also modify the `clean` target at the bottom of `test/Makefile` to clean risk builds:

```make
clean:
	$(CLEAN) *.o $(PROGS)
	$(MAKE) -C risk clean
```

- [ ] **Step 5: Create the initial verification ledger**

Create `docs/risk-verification.md` with this full content:

```markdown
# Risk Verification Ledger

Date: 2026-05-08

Status values: `not run`, `confirmed`, `not reproduced`, `needs environment`,
`documented boundary`, `candidate regression`.

| Risk ID | Priority | Scenario | Command | Status | Evidence |
| --- | --- | --- | --- | --- | --- |
| P0-HOOK-FD-RACE | P0 | Global hook fd table race and UAF | `scripts/risk/run_tsan.sh` | not run | |
| P0-HOOK-CLOSE-STALE | P0 | `close()` leaves stale hook metadata when hook is disabled | `make risk-check` | not run | |
| P0-POLL-SAME-FD | P0 | Two coroutines poll the same fd | `make risk-check` | not run | |
| P0-HOOK-ALLOC-FD-LEAK | P0 | Hook metadata allocation failure leaks fd | `make risk-diagnose` | not run | |
| P0-RESUME-ENDED | P0 | Resume ended coroutine | `make risk-check` | not run | |
| P1-POLL-ZERO-TIMEOUT | P1 | `co_poll(timeout=0)` semantics | `make risk-check` | not run | |
| P1-POLL-FD-SEMANTICS | P1 | Invalid, closed, and regular fd polling semantics | `make risk-check` | not run | |
| P1-CONNECT-ERRNO | P1 | Hooked `connect()` errno behavior | `make risk-check` | not run | |
| P1-SETSOCKOPT-INVALID | P1 | Invalid timeout `setsockopt()` arguments | `make risk-check` | not run | |
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
```

- [ ] **Step 6: Run the empty harness**

Run:

```bash
make risk-check
make risk-diagnose
```

Expected:

```text
No risk-check programs configured
No risk-diagnose programs configured
```

- [ ] **Step 7: Commit the harness**

Run:

```bash
git add Makefile test/Makefile test/risk/Makefile test/risk/risk_common.h docs/risk-verification.md
git commit -m "test: add risk verification harness"
```

## Task 2: Add Stable Poll Semantics Risk Checks

**Files:**
- Create: `test/risk/test_poll_semantics.cpp`
- Modify: `test/risk/Makefile`
- Modify: `docs/risk-verification.md`

- [ ] **Step 1: Add the poll semantics risk program**

Create `test/risk/test_poll_semantics.cpp` with this full content:

```cpp
#include "risk_common.h"
#include "co_routine.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

using namespace co;

struct SameFdState {
  int read_fd{-1};
  int write_fd{-1};
  bool waiter_ready[2]{false, false};
  bool wrote{false};
  int complete{0};
  int ret[2]{-99, -99};
  short revents[2]{0, 0};
  unsigned long long start_ms{0};
};

static int same_fd_loop(void *arg) {
  SameFdState *state = static_cast<SameFdState *>(arg);
  if (state->waiter_ready[0] && state->waiter_ready[1] && !state->wrote) {
    const char byte = 'x';
    write(state->write_fd, &byte, 1);
    state->wrote = true;
  }
  if (state->complete == 2) {
    return -1;
  }
  if (GetTickMS() - state->start_ms > 500) {
    return -1;
  }
  return 0;
}

static risk::Result same_fd_two_waiters() {
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    return risk::needs_environment(
        "P0-POLL-SAME-FD", "two coroutines poll the same fd",
        "pipe can be created", std::string("pipe failed: ") + strerror(errno),
        "risk-check");
  }

  SameFdState state;
  state.read_fd = pipefd[0];
  state.write_fd = pipefd[1];
  state.start_ms = GetTickMS();

  Coroutine *first = co_create([&state]() {
    struct pollfd pfd = {state.read_fd, POLLIN, 0};
    state.waiter_ready[0] = true;
    state.ret[0] = co_poll(&pfd, 1, 50);
    state.revents[0] = pfd.revents;
    ++state.complete;
  });
  Coroutine *second = co_create([&state]() {
    struct pollfd pfd = {state.read_fd, POLLIN, 0};
    state.waiter_ready[1] = true;
    state.ret[1] = co_poll(&pfd, 1, 50);
    state.revents[1] = pfd.revents;
    ++state.complete;
  });

  co_resume(first);
  co_resume(second);
  co_eventloop(same_fd_loop, &state);

  close(pipefd[0]);
  close(pipefd[1]);
  co_free(first);
  co_free(second);

  bool both_ready = state.ret[0] == 1 && state.ret[1] == 1 &&
                    (state.revents[0] & POLLIN) &&
                    (state.revents[1] & POLLIN);
  char actual[256];
  snprintf(actual, sizeof(actual),
           "ret=[%d,%d] revents=[0x%x,0x%x] complete=%d wrote=%d",
           state.ret[0], state.ret[1], state.revents[0], state.revents[1],
           state.complete, state.wrote ? 1 : 0);
  if (!both_ready) {
    return risk::confirmed(
        "P0-POLL-SAME-FD", "two coroutines poll the same fd",
        "both waiters observe POLLIN and return 1", actual, "risk-check");
  }
  return risk::not_reproduced(
      "P0-POLL-SAME-FD", "two coroutines poll the same fd",
      "both waiters observe POLLIN and return 1", actual, "risk-check");
}

static risk::Result zero_timeout_child_check() {
  int status = risk::run_child([]() {
    int pipefd[2];
    risk::require_syscall(pipe(pipefd) == 0, "pipe");
    struct pollfd pfd = {pipefd[0], POLLIN, 0};
    int ret = co_poll(&pfd, 1, 0);
    close(pipefd[0]);
    close(pipefd[1]);
    _exit(ret == 0 ? 0 : 3);
  });
  std::string actual = risk::child_status_text(status);
  if (!risk::child_exited_cleanly(status)) {
    return risk::confirmed(
        "P1-POLL-ZERO-TIMEOUT", "`co_poll(timeout=0)` semantics",
        "return immediately like system poll without aborting or yielding",
        actual, "risk-check");
  }
  return risk::not_reproduced(
      "P1-POLL-ZERO-TIMEOUT", "`co_poll(timeout=0)` semantics",
      "return immediately like system poll without aborting or yielding",
      actual, "risk-check");
}

struct PollOnceState {
  struct pollfd pfd;
  int timeout_ms{5};
  int ret{-99};
  short revents{0};
  bool done{false};
};

static int poll_once_loop(void *arg) {
  PollOnceState *state = static_cast<PollOnceState *>(arg);
  return state->done ? -1 : 0;
}

static void run_co_poll_once(PollOnceState *state) {
  Coroutine *routine = co_create([state]() {
    state->ret = co_poll(&state->pfd, 1, state->timeout_ms);
    state->revents = state->pfd.revents;
    state->done = true;
  });
  co_resume(routine);
  co_eventloop(poll_once_loop, state);
  co_free(routine);
}

static risk::Result closed_fd_poll_semantics() {
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    return risk::needs_environment(
        "P1-POLL-FD-SEMANTICS", "closed fd polling semantics",
        "pipe can be created", std::string("pipe failed: ") + strerror(errno),
        "risk-check");
  }
  int closed_fd = pipefd[0];
  close(pipefd[0]);
  close(pipefd[1]);

  struct pollfd sys_pfd = {closed_fd, POLLIN, 0};
  int sys_ret = poll(&sys_pfd, 1, 0);

  PollOnceState co_state;
  co_state.pfd = {closed_fd, POLLIN, 0};
  run_co_poll_once(&co_state);

  char actual[256];
  snprintf(actual, sizeof(actual),
           "system ret=%d revents=0x%x; co_poll ret=%d revents=0x%x",
           sys_ret, sys_pfd.revents, co_state.ret, co_state.revents);
  bool matches = sys_ret == co_state.ret && sys_pfd.revents == co_state.revents;
  if (!matches) {
    return risk::confirmed(
        "P1-POLL-FD-SEMANTICS", "closed fd polling semantics",
        "match system poll return value and POLLNVAL revents", actual,
        "risk-check");
  }
  return risk::not_reproduced(
      "P1-POLL-FD-SEMANTICS", "closed fd polling semantics",
      "match system poll return value and POLLNVAL revents", actual,
      "risk-check");
}

static risk::Result regular_fd_poll_semantics() {
  int fd = open("/dev/null", O_RDONLY);
  if (fd < 0) {
    return risk::needs_environment(
        "P1-POLL-FD-SEMANTICS", "regular fd polling semantics",
        "/dev/null can be opened", std::string("open failed: ") + strerror(errno),
        "risk-check");
  }

  struct pollfd sys_pfd = {fd, POLLIN, 0};
  int sys_ret = poll(&sys_pfd, 1, 0);

  PollOnceState co_state;
  co_state.pfd = {fd, POLLIN, 0};
  run_co_poll_once(&co_state);
  close(fd);

  char actual[256];
  snprintf(actual, sizeof(actual),
           "system ret=%d revents=0x%x; co_poll ret=%d revents=0x%x",
           sys_ret, sys_pfd.revents, co_state.ret, co_state.revents);
  bool matches = sys_ret == co_state.ret && sys_pfd.revents == co_state.revents;
  if (!matches) {
    return risk::confirmed(
        "P1-POLL-FD-SEMANTICS", "regular fd polling semantics",
        "match system poll readiness for regular files", actual,
        "risk-check");
  }
  return risk::not_reproduced(
      "P1-POLL-FD-SEMANTICS", "regular fd polling semantics",
      "match system poll readiness for regular files", actual,
      "risk-check");
}

int main() {
  std::vector<risk::Result> results;
  results.push_back(same_fd_two_waiters());
  results.push_back(zero_timeout_child_check());
  results.push_back(closed_fd_poll_semantics());
  results.push_back(regular_fd_poll_semantics());
  return risk::summarize(results);
}
```

- [ ] **Step 2: Register the poll risk program**

Modify `test/risk/Makefile` so the program lists become:

```make
RISK_CHECK_PROGS := test_poll_semantics
RISK_DIAGNOSE_PROGS :=
ALL_PROGS := $(RISK_CHECK_PROGS) $(RISK_DIAGNOSE_PROGS)
```

- [ ] **Step 3: Build the poll risk program**

Run:

```bash
make -C test/risk build/test_poll_semantics
```

Expected: the command exits 0 and creates `test/risk/build/test_poll_semantics`.

- [ ] **Step 4: Run the poll risk checks**

Run:

```bash
make risk-check
```

Expected on current HEAD: nonzero exit if any poll risk is confirmed. Output includes `RISK-ID: P0-POLL-SAME-FD`, `RISK-ID: P1-POLL-ZERO-TIMEOUT`, and `RISK-ID: P1-POLL-FD-SEMANTICS`.

- [ ] **Step 5: Update the ledger from observed output**

Modify the rows for `P0-POLL-SAME-FD`, `P1-POLL-ZERO-TIMEOUT`, and `P1-POLL-FD-SEMANTICS` in `docs/risk-verification.md`. Use the actual status printed by the executable and set evidence to `make risk-check`.

- [ ] **Step 6: Commit poll risk checks**

Run:

```bash
git add test/risk/Makefile test/risk/test_poll_semantics.cpp docs/risk-verification.md
git commit -m "test: add poll risk verification"
```

## Task 3: Add Hook Syscall Semantics Risk Checks

**Files:**
- Create: `test/risk/test_hook_syscall_semantics.cpp`
- Modify: `test/risk/Makefile`
- Modify: `docs/risk-verification.md`

- [ ] **Step 1: Add the hook syscall risk program**

Create `test/risk/test_hook_syscall_semantics.cpp` with this full content:

```cpp
#include "risk_common.h"
#include "co_routine.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <string>
#include <vector>

using namespace co;

struct BoolState {
  bool done{false};
  bool confirmed{false};
  std::string actual;
};

static int bool_loop(void *arg) {
  BoolState *state = static_cast<BoolState *>(arg);
  return state->done ? -1 : 0;
}

static risk::Result stale_close_metadata() {
  BoolState state;
  Coroutine *routine = co_create([&state]() {
    co_enable_hook_sys();
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      state.actual = std::string("socket failed: ") + strerror(errno);
      state.done = true;
      return;
    }
    int old_fd = fd;
    co_disable_hook_sys();
    close(fd);

    int reused = -1;
    for (int i = 0; i < 32; ++i) {
      int candidate = open("/dev/null", O_RDONLY);
      if (candidate < 0) {
        state.actual = std::string("open failed: ") + strerror(errno);
        state.done = true;
        return;
      }
      if (candidate == old_fd) {
        reused = candidate;
        break;
      }
      close(candidate);
    }
    if (reused < 0) {
      state.actual = "fd was not reused within 32 open attempts";
      state.done = true;
      return;
    }

    int flags = syscall(SYS_fcntl, reused, F_GETFL, 0);
    syscall(SYS_fcntl, reused, F_SETFL, flags | O_NONBLOCK);
    co_enable_hook_sys();
    int visible_flags = fcntl(reused, F_GETFL, 0);
    state.confirmed = visible_flags >= 0 && (visible_flags & O_NONBLOCK) == 0;

    char buf[160];
    snprintf(buf, sizeof(buf),
             "old_fd=%d reused=%d real_flags=0x%x visible_flags=0x%x",
             old_fd, reused, flags | O_NONBLOCK, visible_flags);
    state.actual = buf;
    close(reused);
    state.done = true;
  });

  co_resume(routine);
  co_eventloop(bool_loop, &state);
  co_free(routine);

  if (state.confirmed) {
    return risk::confirmed(
        "P0-HOOK-CLOSE-STALE",
        "`close()` leaves stale hook metadata when hook is disabled",
        "new fd does not inherit old hook metadata", state.actual,
        "risk-check");
  }
  return risk::not_reproduced(
      "P0-HOOK-CLOSE-STALE",
      "`close()` leaves stale hook metadata when hook is disabled",
      "new fd does not inherit old hook metadata", state.actual, "risk-check");
}

static risk::Result invalid_setsockopt_child() {
  int status = risk::run_child([]() {
    Coroutine *routine = co_create([]() {
      co_enable_hook_sys();
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      risk::require_syscall(fd >= 0, "socket");
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, nullptr, 0);
      close(fd);
    });
    co_resume(routine);
    co_free(routine);
  });

  std::string actual = risk::child_status_text(status);
  if (!risk::child_exited_cleanly(status)) {
    return risk::confirmed(
        "P1-SETSOCKOPT-INVALID",
        "invalid timeout `setsockopt()` arguments",
        "invalid option pointer and length do not crash before syscall",
        actual, "risk-check");
  }
  return risk::not_reproduced(
      "P1-SETSOCKOPT-INVALID", "invalid timeout `setsockopt()` arguments",
      "invalid option pointer and length do not crash before syscall", actual,
      "risk-check");
}

static int bind_unused_local_port() {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  socklen_t len = sizeof(addr);
  if (getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
    close(fd);
    return -1;
  }
  int port = ntohs(addr.sin_port);
  close(fd);
  return port;
}

static risk::Result connect_errno_refused() {
  int port = bind_unused_local_port();
  if (port <= 0) {
    return risk::needs_environment(
        "P1-CONNECT-ERRNO", "hooked `connect()` errno behavior",
        "local unused TCP port can be selected",
        std::string("bind unused port failed: ") + strerror(errno),
        "risk-check");
  }

  BoolState state;
  Coroutine *routine = co_create([&state, port]() {
    co_enable_hook_sys();
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      state.actual = std::string("socket failed: ") + strerror(errno);
      state.done = true;
      return;
    }
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    errno = 0;
    int ret = connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    int saved_errno = errno;
    close(fd);

    char buf[128];
    snprintf(buf, sizeof(buf), "connect ret=%d errno=%d (%s)", ret,
             saved_errno, strerror(saved_errno));
    state.actual = buf;
    state.confirmed = ret < 0 && saved_errno != ECONNREFUSED;
    state.done = true;
  });

  co_resume(routine);
  co_eventloop(bool_loop, &state);
  co_free(routine);

  if (state.confirmed) {
    return risk::confirmed(
        "P1-CONNECT-ERRNO", "hooked `connect()` errno behavior",
        "connection refused is reported as ECONNREFUSED", state.actual,
        "risk-check");
  }
  return risk::not_reproduced(
      "P1-CONNECT-ERRNO", "hooked `connect()` errno behavior",
      "connection refused is reported as ECONNREFUSED", state.actual,
      "risk-check");
}

int main() {
  std::vector<risk::Result> results;
  results.push_back(stale_close_metadata());
  results.push_back(invalid_setsockopt_child());
  results.push_back(connect_errno_refused());
  return risk::summarize(results);
}
```

- [ ] **Step 2: Register the hook syscall risk program**

Modify `test/risk/Makefile` so the risk-check list includes both programs:

```make
RISK_CHECK_PROGS := test_poll_semantics test_hook_syscall_semantics
RISK_DIAGNOSE_PROGS :=
ALL_PROGS := $(RISK_CHECK_PROGS) $(RISK_DIAGNOSE_PROGS)
```

- [ ] **Step 3: Build the hook syscall risk program**

Run:

```bash
make -C test/risk build/test_hook_syscall_semantics
```

Expected: the command exits 0 and creates `test/risk/build/test_hook_syscall_semantics`.

- [ ] **Step 4: Run hook syscall checks**

Run:

```bash
test/risk/build/test_hook_syscall_semantics
```

Expected on current HEAD: output includes `P0-HOOK-CLOSE-STALE`, `P1-SETSOCKOPT-INVALID`, and `P1-CONNECT-ERRNO`; the program exits nonzero if any risk is confirmed.

- [ ] **Step 5: Update the ledger from observed output**

Modify `docs/risk-verification.md` rows for `P0-HOOK-CLOSE-STALE`, `P1-SETSOCKOPT-INVALID`, and `P1-CONNECT-ERRNO` with the actual status and evidence command.

- [ ] **Step 6: Commit hook syscall risk checks**

Run:

```bash
git add test/risk/Makefile test/risk/test_hook_syscall_semantics.cpp docs/risk-verification.md
git commit -m "test: add hook syscall risk verification"
```

## Task 4: Add Lifecycle and API Boundary Risk Checks

**Files:**
- Create: `test/risk/test_lifecycle_boundaries.cpp`
- Modify: `test/risk/Makefile`
- Modify: `docs/risk-verification.md`

- [ ] **Step 1: Add the lifecycle boundary risk program**

Create `test/risk/test_lifecycle_boundaries.cpp` with this full content:

```cpp
#include "risk_common.h"
#include "co_async.h"
#include "co_future.h"
#include "co_routine.h"

#include <stdexcept>
#include <string>
#include <vector>

using namespace co;

static risk::Result resume_ended_coroutine() {
  int status = risk::run_child([]() {
    Coroutine *routine = co_create([]() {});
    co_resume(routine);
    co_resume(routine);
    co_free(routine);
  });
  std::string actual = risk::child_status_text(status);
  if (!risk::child_exited_cleanly(status)) {
    return risk::confirmed(
        "P0-RESUME-ENDED", "resume ended coroutine",
        "second resume is rejected safely or is a documented no-op", actual,
        "risk-check");
  }
  return risk::not_reproduced(
      "P0-RESUME-ENDED", "resume ended coroutine",
      "second resume is rejected safely or is a documented no-op", actual,
      "risk-check");
}

static risk::Result future_without_context() {
  int status = risk::run_child([]() {
    Promise<int> promise;
    Future<int> future = promise.get_future();
    (void)future.get();
  });
  std::string actual = risk::child_status_text(status);
  if (!risk::child_exited_cleanly(status)) {
    return risk::confirmed(
        "P1-FUTURE-NO-CONTEXT", "`Future::get()` outside coroutine context",
        "not-ready future reports an error without assert, abort, or hang",
        actual, "risk-check");
  }
  return risk::not_reproduced(
      "P1-FUTURE-NO-CONTEXT", "`Future::get()` outside coroutine context",
      "not-ready future reports an error without assert, abort, or hang",
      actual, "risk-check");
}

static risk::Result abandoned_promise_behavior() {
  int status = risk::run_child([]() {
    Future<int> *future_ptr = nullptr;
    {
      Promise<int> promise;
      Future<int> future = promise.get_future();
      future_ptr = new Future<int>(std::move(future));
    }
    (void)future_ptr->get();
    delete future_ptr;
  });
  std::string actual = risk::child_status_text(status);
  if (!risk::child_exited_cleanly(status)) {
    return risk::confirmed(
        "P1-PROMISE-ABANDONED", "abandoned promise behavior",
        "future observes a clear broken-promise result without abort or hang",
        actual, "risk-check");
  }
  return risk::not_reproduced(
      "P1-PROMISE-ABANDONED", "abandoned promise behavior",
      "future observes a clear broken-promise result without abort or hang",
      actual, "risk-check");
}

static risk::Result coroutine_throw_boundary() {
  int status = risk::run_child([]() {
    Coroutine *routine = co_create([]() { throw std::runtime_error("boom"); });
    co_resume(routine);
    co_free(routine);
  });
  std::string actual = risk::child_status_text(status);
  if (!risk::child_exited_cleanly(status)) {
    return risk::confirmed(
        "P1-COROUTINE-THROW", "exception crossing coroutine boundary",
        "exception is caught and reported inside coroutine boundary", actual,
        "risk-check");
  }
  return risk::not_reproduced(
      "P1-COROUTINE-THROW", "exception crossing coroutine boundary",
      "exception is caught and reported inside coroutine boundary", actual,
      "risk-check");
}

int main() {
  std::vector<risk::Result> results;
  results.push_back(resume_ended_coroutine());
  results.push_back(future_without_context());
  results.push_back(abandoned_promise_behavior());
  results.push_back(coroutine_throw_boundary());
  return risk::summarize(results);
}
```

- [ ] **Step 2: Register the lifecycle risk program**

Modify `test/risk/Makefile` so the risk-check list includes the lifecycle program:

```make
RISK_CHECK_PROGS := test_poll_semantics test_hook_syscall_semantics test_lifecycle_boundaries
RISK_DIAGNOSE_PROGS :=
ALL_PROGS := $(RISK_CHECK_PROGS) $(RISK_DIAGNOSE_PROGS)
```

- [ ] **Step 3: Build the lifecycle risk program**

Run:

```bash
make -C test/risk build/test_lifecycle_boundaries
```

Expected: the command exits 0 and creates `test/risk/build/test_lifecycle_boundaries`.

- [ ] **Step 4: Run lifecycle checks**

Run:

```bash
test/risk/build/test_lifecycle_boundaries
```

Expected on current HEAD: output includes `P0-RESUME-ENDED`, `P1-FUTURE-NO-CONTEXT`, `P1-PROMISE-ABANDONED`, and `P1-COROUTINE-THROW`; the program exits nonzero if any risk is confirmed.

- [ ] **Step 5: Update the ledger from observed output**

Modify `docs/risk-verification.md` rows for the four lifecycle risks with the actual status and evidence command.

- [ ] **Step 6: Commit lifecycle risk checks**

Run:

```bash
git add test/risk/Makefile test/risk/test_lifecycle_boundaries.cpp docs/risk-verification.md
git commit -m "test: add lifecycle risk verification"
```

## Task 5: Add Sanitizer and Boundary Diagnostics

**Files:**
- Create: `test/risk/diag_hook_fd_race.cpp`
- Create: `test/risk/diag_leaks_and_boundaries.cpp`
- Create: `test/risk/diag_perf_boundaries.cpp`
- Modify: `test/risk/Makefile`
- Create: `scripts/risk/run_tsan.sh`
- Create: `scripts/risk/run_asan_lsan.sh`
- Create: `scripts/risk/run_strace_poll.sh`
- Modify: `docs/risk-verification.md`

- [ ] **Step 1: Add the hook fd race diagnostic**

Create `test/risk/diag_hook_fd_race.cpp` with this full content:

```cpp
#include "risk_common.h"
#include "co_routine.h"

#include <fcntl.h>
#include <stdio.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace co;

static int create_hooked_socket_fd() {
  int fd = -1;
  Coroutine *routine = co_create([&fd]() {
    co_enable_hook_sys();
    fd = socket(AF_INET, SOCK_STREAM, 0);
  });
  co_resume(routine);
  co_free(routine);
  return fd;
}

static void hooked_reader(int fd, int rounds) {
  Coroutine *routine = co_create([fd, rounds]() {
    co_enable_hook_sys();
    for (int i = 0; i < rounds; ++i) {
      (void)fcntl(fd, F_GETFL, 0);
    }
  });
  co_resume(routine);
  co_free(routine);
}

static void hooked_closer(int fd) {
  Coroutine *routine = co_create([fd]() {
    co_enable_hook_sys();
    close(fd);
  });
  co_resume(routine);
  co_free(routine);
}

int main() {
  int fd = create_hooked_socket_fd();
  if (fd < 0) {
    printf("RISK-ID: P0-HOOK-FD-RACE\n");
    printf("status: needs environment\n");
    printf("actual: failed to create hooked socket\n");
    return 0;
  }

  std::vector<std::thread> threads;
  for (int i = 0; i < 6; ++i) {
    threads.emplace_back(hooked_reader, fd, 20000);
  }
  threads.emplace_back(hooked_closer, fd);
  for (std::thread &thread : threads) {
    thread.join();
  }

  printf("RISK-ID: P0-HOOK-FD-RACE\n");
  printf("scenario: multi-thread hook fd table access\n");
  printf("expected: no TSan data race, no UAF, no stale metadata\n");
  printf("actual: diagnostic completed; inspect sanitizer output\n");
  printf("status: not reproduced without sanitizer report\n");
  printf("regression: risk-diagnose only\n");
  return 0;
}
```

- [ ] **Step 2: Add leak and boundary diagnostics**

Create `test/risk/diag_leaks_and_boundaries.cpp` with this full content:

```cpp
#include "risk_common.h"
#include "co_async.h"
#include "co_cond.h"
#include "co_routine.h"
#include "task.h"
#include "thread_worker.h"

#include <atomic>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace co;

static void run_env_leak_probe() {
  const char *names[] = {"LIBCO_RISK_ENV"};
  co_set_env_list(names, 1);
  for (int i = 0; i < 100; ++i) {
    Coroutine *routine = co_create([]() {
      co_enable_hook_sys();
      setenv("LIBCO_RISK_ENV", "value", 1);
      (void)getenv("LIBCO_RISK_ENV");
    });
    co_resume(routine);
    co_free(routine);
  }
}

static void run_thread_env_probe() {
  int before = risk::count_open_fds();
  for (int i = 0; i < 50; ++i) {
    std::thread([]() {
      Coroutine *routine = co_create([]() {});
      co_resume(routine);
      co_free(routine);
    }).join();
  }
  int after = risk::count_open_fds();
  printf("RISK-ID: P1-THREADENV-LEAK\n");
  printf("scenario: per-thread ThreadEnv leak\n");
  printf("expected: fd count does not grow after short-lived coroutine threads\n");
  printf("actual: fd_count_before=%d fd_count_after=%d\n", before, after);
  printf("status: %s\n", after > before ? "confirmed" : "not reproduced");
  printf("regression: risk-diagnose\n\n");
}

static void run_hook_alloc_fd_leak_probe() {
  struct rlimit limit;
  if (getrlimit(RLIMIT_NOFILE, &limit) != 0 || limit.rlim_cur <= 102401) {
    printf("RISK-ID: P0-HOOK-ALLOC-FD-LEAK\n");
    printf("scenario: hook metadata allocation failure leaks fd\n");
    printf("expected: environment can allocate fd >= 102400\n");
    printf("actual: RLIMIT_NOFILE is below required fd-table ceiling\n");
    printf("status: needs environment\n");
    printf("regression: risk-diagnose\n\n");
    return;
  }

  std::vector<int> fds;
  int fd = -1;
  while (fd < 102400) {
    fd = open("/dev/null", O_RDONLY);
    if (fd < 0) {
      break;
    }
    fds.push_back(fd);
  }

  int before = risk::count_open_fds();
  int hooked_fd = -1;
  Coroutine *routine = co_create([&hooked_fd]() {
    co_enable_hook_sys();
    hooked_fd = socket(AF_INET, SOCK_STREAM, 0);
  });
  co_resume(routine);
  co_free(routine);
  int after = risk::count_open_fds();

  for (int open_fd : fds) {
    close(open_fd);
  }
  if (hooked_fd >= 0) {
    close(hooked_fd);
  }

  printf("RISK-ID: P0-HOOK-ALLOC-FD-LEAK\n");
  printf("scenario: hook metadata allocation failure leaks fd\n");
  printf("expected: failed hooked socket closes the real fd\n");
  printf("actual: hooked_fd=%d fd_count_before=%d fd_count_after=%d\n",
         hooked_fd, before, after);
  printf("status: %s\n", hooked_fd < 0 && after > before ? "confirmed"
                                                         : "not reproduced");
  printf("regression: risk-diagnose\n\n");
}

static void run_allocation_failure_probe() {
  int status = risk::run_child([]() {
    struct rlimit limit;
    limit.rlim_cur = 8 * 1024 * 1024;
    limit.rlim_max = 8 * 1024 * 1024;
    setrlimit(RLIMIT_AS, &limit);
    std::vector<Coroutine *> routines;
    for (int i = 0; i < 100000; ++i) {
      routines.push_back(co_create([]() {}));
    }
    for (Coroutine *routine : routines) {
      co_free(routine);
    }
  });

  printf("RISK-ID: P1-ALLOC-FAILURE\n");
  printf("scenario: allocation failure safety\n");
  printf("expected: allocation failure reports a controlled error\n");
  printf("actual: %s\n", risk::child_status_text(status).c_str());
  printf("status: %s\n", risk::child_exited_cleanly(status)
                            ? "not reproduced"
                            : "confirmed");
  printf("regression: risk-diagnose\n\n");
}

static void run_cond_cross_thread_probe() {
  CoCond cond;
  std::atomic<bool> waiter_started{false};
  std::thread waiter([&]() {
    Coroutine *routine = co_create([&]() {
      waiter_started.store(true, std::memory_order_release);
      cond.Timedwait(50);
    });
    co_resume(routine);
    co_eventloop([](void *) { return 0; }, nullptr);
  });
  while (!waiter_started.load(std::memory_order_acquire)) {
    usleep(1000);
  }
  std::thread signaler([&]() { cond.Signal(); });
  signaler.join();
  pthread_cancel(waiter.native_handle());
  waiter.join();

  printf("RISK-ID: P1-COND-CROSS-THREAD\n");
  printf("scenario: CoCond signal from a different thread\n");
  printf("expected: no TSan race and no wrong-thread resume\n");
  printf("actual: diagnostic completed; inspect sanitizer output\n");
  printf("status: not reproduced without sanitizer report\n");
  printf("regression: risk-diagnose\n\n");
}

static void run_schedule_thread_local_probe() {
  std::atomic<int> ran{0};
  std::thread producer([&]() {
    schedule(make_task([&]() { ran.fetch_add(1, std::memory_order_relaxed); }));
  });
  producer.join();
  ThreadWorker worker(0);
  worker.run_loop(false);

  printf("RISK-ID: P2-SCHEDULE-THREAD-LOCAL\n");
  printf("scenario: schedule from one thread and run worker in another\n");
  printf("expected: API boundary is documented or task executes intentionally\n");
  printf("actual: ran=%d\n", ran.load(std::memory_order_relaxed));
  printf("status: %s\n", ran.load(std::memory_order_relaxed) == 0
                            ? "documented boundary"
                            : "not reproduced");
  printf("regression: risk-diagnose\n\n");
}

int main() {
  run_env_leak_probe();
  printf("RISK-ID: P1-ENV-LEAK\n");
  printf("scenario: coroutine private environment leak\n");
  printf("expected: LSan reports no per-coroutine env leak\n");
  printf("actual: env probe completed; inspect LSan output\n");
  printf("status: not reproduced without leak report\n");
  printf("regression: risk-diagnose\n\n");

  run_thread_env_probe();
  run_hook_alloc_fd_leak_probe();
  run_allocation_failure_probe();
  run_cond_cross_thread_probe();
  run_schedule_thread_local_probe();
  return 0;
}
```

- [ ] **Step 3: Add performance diagnostics**

Create `test/risk/diag_perf_boundaries.cpp` with this full content:

```cpp
#include "risk_common.h"
#include "co_routine.h"
#include "task.h"
#include "thread_worker.h"
#include "util.h"

#include <stdio.h>
#include <sys/resource.h>
#include <unistd.h>

using namespace co;

struct IdleState {
  unsigned long long start_ms{0};
  int duration_ms{200};
};

static int idle_loop(void *arg) {
  IdleState *state = static_cast<IdleState *>(arg);
  return GetTickMS() - state->start_ms >=
                 static_cast<unsigned long long>(state->duration_ms)
             ? -1
             : 0;
}

static void run_idle_cpu_probe() {
  struct rusage before;
  struct rusage after;
  getrusage(RUSAGE_SELF, &before);
  IdleState state;
  state.start_ms = GetTickMS();
  co_eventloop(idle_loop, &state);
  getrusage(RUSAGE_SELF, &after);

  long before_us = before.ru_utime.tv_sec * 1000000L + before.ru_utime.tv_usec +
                   before.ru_stime.tv_sec * 1000000L + before.ru_stime.tv_usec;
  long after_us = after.ru_utime.tv_sec * 1000000L + after.ru_utime.tv_usec +
                  after.ru_stime.tv_sec * 1000000L + after.ru_stime.tv_usec;
  printf("RISK-ID: P2-IDLE-CPU\n");
  printf("scenario: empty event loop idle CPU\n");
  printf("expected: idle loop consumes little CPU while waiting\n");
  printf("actual: cpu_us=%ld duration_ms=%d\n", after_us - before_us,
         state.duration_ms);
  printf("status: %s\n", (after_us - before_us) > 50000 ? "confirmed"
                                                        : "not reproduced");
  printf("regression: risk-diagnose\n\n");
}

static void run_run_loop_cleanup_probe() {
  static int destructed = 0;
  struct Counter {
    ~Counter() { ++destructed; }
  };
  {
    Counter counter;
    schedule(make_task([counter]() mutable {}));
  }
  ThreadWorker worker(0);
  worker.run_loop(false);
  int after_first = destructed;
  worker.run_loop(false);
  int after_second = destructed;

  printf("RISK-ID: P2-RUN-LOOP-CLEANUP\n");
  printf("scenario: run_loop(false) final coroutine cleanup\n");
  printf("expected: completed task coroutine is cleaned in the same run\n");
  printf("actual: destructed_after_first=%d destructed_after_second=%d\n",
         after_first, after_second);
  printf("status: %s\n", after_second > after_first ? "confirmed"
                                                    : "not reproduced");
  printf("regression: risk-diagnose\n\n");
}

int main() {
  run_idle_cpu_probe();
  run_run_loop_cleanup_probe();
  printf("RISK-ID: P2-LONG-TIMEOUT\n");
  printf("scenario: timeout wheel long-timeout behavior\n");
  printf("expected: long timeout does not require repeated unexpected wakeups\n");
  printf("actual: manual long-duration run excluded from short diagnostic binary\n");
  printf("status: needs environment\n");
  printf("regression: risk-diagnose\n\n");
  printf("RISK-ID: P2-PLATFORM-GUARD\n");
  printf("scenario: platform and ABI guard\n");
  printf("expected: unsupported platforms fail at configure time or are documented\n");
  printf("actual: inspect CMakeLists.txt and Makefile for -m64 and architecture guards\n");
  printf("status: documented boundary\n");
  printf("regression: manual inspection\n");
  return 0;
}
```

- [ ] **Step 4: Register diagnostic programs**

Modify `test/risk/Makefile` so the program lists become:

```make
RISK_CHECK_PROGS := test_poll_semantics test_hook_syscall_semantics test_lifecycle_boundaries
RISK_DIAGNOSE_PROGS := diag_hook_fd_race diag_leaks_and_boundaries diag_perf_boundaries
ALL_PROGS := $(RISK_CHECK_PROGS) $(RISK_DIAGNOSE_PROGS)
```

- [ ] **Step 5: Add the TSan script**

Create `scripts/risk/run_tsan.sh` with this full content:

```bash
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

if grep -q "WARNING: ThreadSanitizer" "$LOG_DIR/P0-HOOK-FD-RACE.tsan.log" ||
   grep -q "WARNING: ThreadSanitizer" "$LOG_DIR/P1-COND-CROSS-THREAD.tsan.log"; then
  exit 1
fi

if [ "$race_status" -ne 0 ] || [ "$cond_status" -ne 0 ]; then
  exit 1
fi
```

- [ ] **Step 6: Add the ASan/LSan script**

Create `scripts/risk/run_asan_lsan.sh` with this full content:

```bash
#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LOG_DIR="$ROOT/logs/risk"
mkdir -p "$LOG_DIR"

make -C "$ROOT/test/risk" clean
make -C "$ROOT/test/risk" all \
  SAN_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"

set +e
ASAN_OPTIONS="detect_leaks=1:abort_on_error=0" \
  "$ROOT/test/risk/build/test_lifecycle_boundaries" >"$LOG_DIR/P0-P1-lifecycle.asan.log" 2>&1
lifecycle_status=$?
ASAN_OPTIONS="detect_leaks=1:abort_on_error=0" \
  "$ROOT/test/risk/build/diag_leaks_and_boundaries" >"$LOG_DIR/P1-leaks.asan-lsan.log" 2>&1
leak_status=$?
set -e

echo "ASan lifecycle log: $LOG_DIR/P0-P1-lifecycle.asan.log"
echo "LSan leak log: $LOG_DIR/P1-leaks.asan-lsan.log"

if grep -q "ERROR: AddressSanitizer" "$LOG_DIR/P0-P1-lifecycle.asan.log" ||
   grep -q "ERROR: LeakSanitizer" "$LOG_DIR/P1-leaks.asan-lsan.log"; then
  exit 1
fi

if [ "$lifecycle_status" -ne 0 ] || [ "$leak_status" -ne 0 ]; then
  exit 1
fi
```

- [ ] **Step 7: Add the strace script**

Create `scripts/risk/run_strace_poll.sh` with this full content:

```bash
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

echo "strace log: $LOG_DIR/P0-POLL-SAME-FD.strace.log"
exit "$status"
```

- [ ] **Step 8: Make scripts executable**

Run:

```bash
chmod +x scripts/risk/run_tsan.sh scripts/risk/run_asan_lsan.sh scripts/risk/run_strace_poll.sh
```

- [ ] **Step 9: Build diagnostics**

Run:

```bash
make -C test/risk build/diag_hook_fd_race build/diag_leaks_and_boundaries build/diag_perf_boundaries
```

Expected: the command exits 0 and creates all three diagnostic binaries.

- [ ] **Step 10: Run normal diagnostics target**

Run:

```bash
make risk-diagnose
```

Expected: diagnostic programs print risk records. The target exits nonzero if a diagnostic reports confirmed risk through its exit status.

- [ ] **Step 11: Run sanitizer scripts**

Run:

```bash
scripts/risk/run_tsan.sh
scripts/risk/run_asan_lsan.sh
```

Expected on current HEAD: scripts may exit nonzero when sanitizer output confirms a risk. Logs are written under `logs/risk`.

- [ ] **Step 12: Run strace script if available**

Run:

```bash
scripts/risk/run_strace_poll.sh
```

Expected when `strace` is installed: logs are written under `logs/risk`. If `strace` is not installed, the script exits 2 and prints `strace is not installed`.

- [ ] **Step 13: Update the ledger from diagnostic output**

Modify `docs/risk-verification.md` rows for `P0-HOOK-FD-RACE`, `P0-HOOK-ALLOC-FD-LEAK`, `P1-ENV-LEAK`, `P1-THREADENV-LEAK`, `P1-COND-CROSS-THREAD`, `P1-ALLOC-FAILURE`, `P2-IDLE-CPU`, `P2-LONG-TIMEOUT`, `P2-RUN-LOOP-CLEANUP`, `P2-SCHEDULE-THREAD-LOCAL`, and `P2-PLATFORM-GUARD`. Use observed output and log paths under `logs/risk`.

- [ ] **Step 14: Commit diagnostics**

Run:

```bash
git add test/risk/Makefile test/risk/diag_hook_fd_race.cpp test/risk/diag_leaks_and_boundaries.cpp test/risk/diag_perf_boundaries.cpp scripts/risk/run_tsan.sh scripts/risk/run_asan_lsan.sh scripts/risk/run_strace_poll.sh docs/risk-verification.md
git commit -m "test: add risk diagnostics"
```

## Task 6: Final Verification Pass and Documentation Review

**Files:**
- Modify: `docs/risk-verification.md`
- Modify: `docs/superpowers/specs/2026-05-08-risk-verification-design.md` only if execution reveals a contradiction in the approved design

- [ ] **Step 1: Run baseline tests**

Run:

```bash
make check
```

Expected: existing tests pass. If this fails, stop and record the failure before changing risk code.

- [ ] **Step 2: Run stable risk checks**

Run:

```bash
make risk-check
```

Expected on current HEAD: target may exit nonzero because confirmed risks are present. Every risk-check executable must print structured records with `RISK-ID`, `scenario`, `expected`, `actual`, `status`, and `regression`.

- [ ] **Step 3: Run diagnostic checks**

Run:

```bash
make risk-diagnose
```

Expected on current HEAD: target may exit nonzero when diagnostics confirm risks. Diagnostic output must be sufficient to update `docs/risk-verification.md`.

- [ ] **Step 4: Run optional sanitizer and trace wrappers**

Run:

```bash
scripts/risk/run_tsan.sh
scripts/risk/run_asan_lsan.sh
scripts/risk/run_strace_poll.sh
```

Expected: scripts create logs under `logs/risk`. If `strace` is missing, record `needs environment` for the trace evidence and keep the compiled risk-check evidence.

- [ ] **Step 5: Ensure ledger has no `not run` rows for implemented checks**

Run:

```bash
rg "not run" docs/risk-verification.md
```

Expected: rows that have implemented checks no longer say `not run`. Platform-specific or resource-limit checks may say `needs environment` with a reason in the Evidence column.

- [ ] **Step 6: Check formatting**

Run:

```bash
git diff --check
```

Expected: no whitespace errors.

- [ ] **Step 7: Check final status**

Run:

```bash
git status --short
```

Expected: only intended risk verification files are modified or untracked. Existing untracked example/test binaries may remain untouched.

- [ ] **Step 8: Commit final ledger updates**

Run:

```bash
git add docs/risk-verification.md
git commit -m "docs: record risk verification results"
```
