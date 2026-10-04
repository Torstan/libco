#include "risk_common.h"
#include "co_routine.h"
#include "internal/util.h"
#include "internal/io_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <thread>
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

static int same_fd_two_waiters_child(bool hooked_socket) {
  int pipefd[2];
  if ((hooked_socket ? socketpair(AF_UNIX, SOCK_STREAM, 0, pipefd)
                     : pipe(pipefd)) != 0) {
    _exit(2);
  }

  SameFdState state;
  state.read_fd = pipefd[0];
  state.write_fd = pipefd[1];
  state.start_ms = GetTickMS();

  Coroutine *first = co_create([&state, hooked_socket]() {
    if (hooked_socket) {
      co_enable_hook_sys();
    }
    struct pollfd pfd = {state.read_fd, POLLIN, 0};
    state.waiter_ready[0] = true;
    state.ret[0] = hooked_socket ? poll(&pfd, 1, 50) : co_poll(&pfd, 1, 50);
    state.revents[0] = pfd.revents;
    ++state.complete;
  });
  Coroutine *second = co_create([&state, hooked_socket]() {
    if (hooked_socket) {
      co_enable_hook_sys();
    }
    struct pollfd pfd = {state.read_fd, POLLIN, 0};
    state.waiter_ready[1] = true;
    state.ret[1] = hooked_socket ? poll(&pfd, 1, 50) : co_poll(&pfd, 1, 50);
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

static risk::Result same_fd_two_waiters(bool hooked_socket = false) {
  int status = risk::run_child_with_timeout(
      [hooked_socket]() { same_fd_two_waiters_child(hooked_socket); }, 1000);
  const char *scenario = hooked_socket ? "two hooked poll waiters on a socket"
                                      : "two coroutines poll the same fd";
  std::string actual = risk::child_status_text(status);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 2) {
    return risk::needs_environment(
        "P0-POLL-SAME-FD", scenario,
        "pipe or socket pair can be created", actual, "risk-check");
  }
  if (!risk::child_exited_cleanly(status)) {
    return risk::confirmed(
        "P0-POLL-SAME-FD", scenario,
        "both waiters observe POLLIN and return 1", actual, "risk-check");
  }
  return risk::not_reproduced(
      "P0-POLL-SAME-FD", scenario,
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
  bool hooked{false};
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
    if (state->hooked) {
      co_enable_hook_sys();
    }
    state->ret = state->hooked ? poll(&state->pfd, 1, state->timeout_ms)
                               : co_poll(&state->pfd, 1, state->timeout_ms);
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

static risk::Result file_poll_semantics(bool regular_file, bool hooked) {
  FILE *file = regular_file ? tmpfile() : nullptr;
  int fd = regular_file ? (file ? fileno(file) : -1)
                        : open("/dev/null", O_RDONLY);
  const char *scenario = regular_file ? "regular file polling semantics"
                                      : "/dev/null polling semantics";
  if (fd < 0) {
    return risk::needs_environment(
        "P1-POLL-FD-SEMANTICS", scenario,
        "file can be opened", std::string("open failed: ") + strerror(errno),
        "risk-check");
  }

  struct pollfd sys_pfd = {fd, POLLIN, 0};
  int sys_ret = poll(&sys_pfd, 1, 0);

  PollOnceState co_state;
  co_state.pfd = {fd, POLLIN, 0};
  co_state.hooked = hooked;
  run_co_poll_once(&co_state);
  if (file) {
    fclose(file);
  } else {
    close(fd);
  }

  char actual[256];
  snprintf(actual, sizeof(actual),
           "system ret=%d revents=0x%x; co_poll ret=%d revents=0x%x done=%d hooked=%d",
           sys_ret, sys_pfd.revents, co_state.ret, co_state.revents,
           co_state.done ? 1 : 0, hooked ? 1 : 0);
  bool matches = co_state.done && sys_ret == co_state.ret &&
                 sys_pfd.revents == co_state.revents;
  if (!matches) {
    return risk::confirmed(
        "P1-POLL-FD-SEMANTICS", scenario,
        "match system poll return value and revents", actual,
        "risk-check");
  }
  return risk::not_reproduced(
      "P1-POLL-FD-SEMANTICS", scenario,
      "match system poll return value and revents", actual,
      "risk-check");
}

static risk::Result mixed_fd_poll_semantics(bool hooked) {
  int status = risk::run_child_with_timeout([hooked]() {
    co_get_epoll_ct();
    int pipefd[2];
    risk::require_syscall(pipe(pipefd) == 0, "pipe");
    int fd = open("/dev/null", O_RDONLY);
    risk::require_syscall(fd >= 0, "open");
    int closed_fd = dup(pipefd[0]);
    risk::require_syscall(closed_fd >= 0, "dup");
    close(closed_fd);
    risk::require_syscall(write(pipefd[1], "x", 1) == 1, "write");
    pollfd expected[] = {{pipefd[0], POLLIN, 0}, {fd, POLLIN, 0},
                         {closed_fd, POLLIN, 0}, {-1, POLLIN, POLLIN}};
    pollfd actual[4];
    memcpy(actual, expected, sizeof(actual));
    int expected_ret = poll(expected, 4, 0);
    bool done = false;
    bool matches = false;
    Coroutine *routine = co_create([&]() {
      if (hooked) {
        co_enable_hook_sys();
      }
      int ret = hooked ? poll(actual, 4, 50) : co_poll(actual, 4, 50);
      matches = ret == expected_ret && ret > 0;
      for (int i = 0; i < 4; ++i) {
        matches = matches && actual[i].revents == expected[i].revents;
      }
      done = true;
    });
    co_resume(routine);
    // Ready/error entries must be returned without waiting for the event loop.
    _exit(done && matches ? 0 : 1);
  }, 1000);
  const char *scenario = hooked ? "hooked poll with mixed fd types"
                                : "co_poll with mixed fd types";
  if (WIFEXITED(status) && WEXITSTATUS(status) == 2) {
    return risk::needs_environment("P1-POLL-FD-SEMANTICS", scenario,
        "test descriptors can be created", risk::child_status_text(status), "risk-check");
  }
  return risk::child_exited_cleanly(status)
      ? risk::not_reproduced("P1-POLL-FD-SEMANTICS", scenario,
          "return all native poll events immediately", risk::child_status_text(status), "risk-check")
      : risk::confirmed("P1-POLL-FD-SEMANTICS", scenario,
          "return all native poll events immediately", risk::child_status_text(status), "risk-check");
}

static risk::Result registration_error() {
  int status = risk::run_child_with_timeout([]() {
    // Create a fresh thread-local backend instead of using one inherited by fork.
    std::thread([]() {
      EpollCtx *ctx = co_get_epoll_ct();
      int pipefd[2];
      risk::require_syscall(ctx && pipe(pipefd) == 0, "poll setup");
      bool done = false;
      bool matches = false;
      Coroutine *routine = co_create([&]() {
        pollfd fds[2] = {{pipefd[0], POLLIN, 0}, {-1, POLLIN, 0}};
        int ret = co_poll(fds, 2, 50);
        matches = ret == -1 && errno == EBADF;
        if (!matches) {
          fprintf(stderr, "registration failure: ret=%d errno=%d\n", ret, errno);
        }
        done = true;
      });
      close(ctx->fd());
      co_resume(routine);
      if (!done) {
        fprintf(stderr, "registration failure unexpectedly yielded\n");
      }
      _exit(done && matches ? 0 : 1);
    }).join();
  }, 1000);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 2) {
    return risk::needs_environment("P1-POLL-FD-SEMANTICS", "registration failure",
        "test descriptors can be created", risk::child_status_text(status), "risk-check");
  }
  return risk::child_exited_cleanly(status)
      ? risk::not_reproduced("P1-POLL-FD-SEMANTICS", "registration failure",
          "return backend error without yielding", risk::child_status_text(status), "risk-check")
      : risk::confirmed("P1-POLL-FD-SEMANTICS", "registration failure",
          "return backend error without yielding", risk::child_status_text(status), "risk-check");
}

int main() {
  std::vector<risk::Result> results;
  results.push_back(same_fd_two_waiters());
  results.push_back(same_fd_two_waiters(true));
  results.push_back(zero_timeout_child_check());
  results.push_back(closed_fd_poll_semantics());
  for (bool hooked : {false, true}) {
    results.push_back(file_poll_semantics(false, hooked));
    results.push_back(file_poll_semantics(true, hooked));
    results.push_back(mixed_fd_poll_semantics(hooked));
  }
  results.push_back(registration_error());
  return risk::summarize(results);
}
