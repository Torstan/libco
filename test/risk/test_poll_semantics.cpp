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
    ssize_t written = write(state->write_fd, &byte, 1);
    (void)written;
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

static int same_fd_two_waiters_child() {
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    _exit(2);
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
  if (state.complete == 2) {
    co_free(first);
    co_free(second);
  }

  bool both_ready = state.ret[0] == 1 && state.ret[1] == 1 &&
                    (state.revents[0] & POLLIN) &&
                    (state.revents[1] & POLLIN);
  _exit(both_ready ? 0 : 1);
}

static risk::Result same_fd_two_waiters() {
  int status = risk::run_child_with_timeout([]() { same_fd_two_waiters_child(); },
                                            1000);
  std::string actual = risk::child_status_text(status);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 2) {
    return risk::needs_environment(
        "P0-POLL-SAME-FD", "two coroutines poll the same fd",
        "pipe can be created", actual, "risk-check");
  }
  if (!risk::child_exited_cleanly(status)) {
    return risk::confirmed(
        "P0-POLL-SAME-FD", "two coroutines poll the same fd",
        "both waiters observe POLLIN and return 1", actual, "risk-check");
  }
  return risk::not_reproduced(
      "P0-POLL-SAME-FD", "two coroutines poll the same fd",
      "both waiters observe POLLIN and return 1", actual, "risk-check");
}

static risk::Result zero_timeout_child_check() {
  int status = risk::run_child_with_timeout([]() {
    int pipefd[2];
    risk::require_syscall(pipe(pipefd) == 0, "pipe");
    struct pollfd pfd = {pipefd[0], POLLIN, 0};
    int ret = co_poll(&pfd, 1, 0);
    close(pipefd[0]);
    close(pipefd[1]);
    _exit(ret == 0 ? 0 : 3);
  }, 1000);
  std::string actual = risk::child_status_text(status);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 2) {
    return risk::needs_environment(
        "P1-POLL-ZERO-TIMEOUT", "`co_poll(timeout=0)` semantics",
        "pipe can be created", actual, "risk-check");
  }
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
  unsigned long long start_ms{0};
  unsigned long long max_ms{100};
  int ret{-99};
  short revents{0};
  bool done{false};
};

static int poll_once_loop(void *arg) {
  PollOnceState *state = static_cast<PollOnceState *>(arg);
  if (state->done) {
    return -1;
  }
  if (GetTickMS() - state->start_ms >= state->max_ms) {
    return -1;
  }
  return 0;
}

static void run_co_poll_once(PollOnceState *state) {
  state->start_ms = GetTickMS();
  Coroutine *routine = co_create([state]() {
    state->ret = co_poll(&state->pfd, 1, state->timeout_ms);
    state->revents = state->pfd.revents;
    state->done = true;
  });
  co_resume(routine);
  co_eventloop(poll_once_loop, state);
  if (state->done) {
    co_free(routine);
  }
}

static risk::Result closed_fd_poll_semantics() {
  co_get_epoll_ct();

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
           "system ret=%d revents=0x%x; co_poll ret=%d revents=0x%x done=%d",
           sys_ret, sys_pfd.revents, co_state.ret, co_state.revents,
           co_state.done ? 1 : 0);
  bool matches = co_state.done && sys_ret == co_state.ret &&
                 sys_pfd.revents == co_state.revents;
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
           "system ret=%d revents=0x%x; co_poll ret=%d revents=0x%x done=%d",
           sys_ret, sys_pfd.revents, co_state.ret, co_state.revents,
           co_state.done ? 1 : 0);
  bool matches = co_state.done && sys_ret == co_state.ret &&
                 sys_pfd.revents == co_state.revents;
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
