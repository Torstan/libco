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
#include <sys/wait.h>
#include <unistd.h>

#include <functional>
#include <string>
#include <vector>

using namespace co;

namespace {

enum ProbeExit {
  kProbeNotReproduced = 0,
  kProbeConfirmed = 1,
  kProbeNeedsEnvironment = 2,
};

struct ChildProbeResult {
  int status{255};
  std::string output;
};

ChildProbeResult run_child_probe_with_timeout(
    const std::function<void(int)> &fn, int timeout_ms) {
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    perror("pipe");
    return ChildProbeResult{255, "pipe failed"};
  }

  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    close(pipefd[0]);
    close(pipefd[1]);
    return ChildProbeResult{255, "fork failed"};
  }

  if (pid == 0) {
    close(pipefd[0]);
    fn(pipefd[1]);
    close(pipefd[1]);
    _exit(kProbeNotReproduced);
  }

  close(pipefd[1]);

  int status = 0;
  unsigned long long deadline = risk::now_ms() + timeout_ms;
  for (;;) {
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid) {
      break;
    }
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("waitpid");
      status = 255;
      break;
    }
    if (risk::now_ms() >= deadline) {
      kill(pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
          perror("waitpid");
          status = 255;
          break;
        }
      }
      break;
    }
    usleep(1000);
  }

  std::string output;
  char buf[256];
  ssize_t n = 0;
  while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
    output.append(buf, static_cast<size_t>(n));
  }
  close(pipefd[0]);

  if (!output.empty() && output.back() == '\n') {
    output.pop_back();
  }
  if (output.empty()) {
    output = risk::child_status_text(status);
  }
  return ChildProbeResult{status, output};
}

void write_probe_line(int fd, const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0) {
    size_t len = static_cast<size_t>(n);
    if (len >= sizeof(buf)) {
      len = sizeof(buf) - 1;
    }
    ssize_t ignored = write(fd, buf, len);
    (void)ignored;
  }
}

bool probe_exit_code(int status, int code) {
  return WIFEXITED(status) && WEXITSTATUS(status) == code;
}

struct BoolState {
  bool done{false};
  bool confirmed{false};
  bool needs_environment{false};
  bool timed_out{false};
  std::string actual;
  unsigned long long start_ms{0};
};

int bool_loop(void *arg) {
  BoolState *state = static_cast<BoolState *>(arg);
  if (state->done) {
    return -1;
  }
  if (risk::now_ms() - state->start_ms > 1000) {
    state->actual = "event loop exceeded 1000ms";
    state->confirmed = true;
    state->timed_out = true;
    state->done = true;
    return -1;
  }
  return 0;
}

void run_bounded_coroutine(BoolState *state,
                           const std::function<void()> &fn) {
  state->start_ms = risk::now_ms();
  Coroutine *routine = co_create([fn]() { fn(); });
  co_resume(routine);
  co_eventloop(bool_loop, state);
  if (state->done && !state->timed_out) {
    co_free(routine);
  }
}

void stale_close_metadata_child(int out_fd) {
  BoolState state;
  run_bounded_coroutine(&state, [&state]() {
    co_enable_hook_sys();
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      state.actual = std::string("socket failed: ") + strerror(errno);
      state.needs_environment = true;
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
        state.needs_environment = true;
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
      state.needs_environment = true;
      state.done = true;
      return;
    }

    int flags = syscall(SYS_fcntl, reused, F_GETFL, 0);
    if (flags < 0) {
      state.actual = std::string("syscall fcntl F_GETFL failed: ") +
                     strerror(errno);
      close(reused);
      state.needs_environment = true;
      state.done = true;
      return;
    }

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

  write_probe_line(out_fd, "%s", state.actual.c_str());
  if (state.needs_environment) {
    _exit(kProbeNeedsEnvironment);
  }
  _exit(state.confirmed ? kProbeConfirmed : kProbeNotReproduced);
}

risk::Result stale_close_metadata() {
  ChildProbeResult child =
      run_child_probe_with_timeout(stale_close_metadata_child, 1500);
  std::string actual =
      child.output + "; " + risk::child_status_text(child.status);
  if (probe_exit_code(child.status, kProbeNeedsEnvironment)) {
    return risk::needs_environment(
        "P0-HOOK-CLOSE-STALE",
        "`close()` leaves stale hook metadata when hook is disabled",
        "socket and replacement fd can be created", actual, "risk-check");
  }
  if (probe_exit_code(child.status, kProbeConfirmed) ||
      !risk::child_exited_cleanly(child.status)) {
    return risk::confirmed(
        "P0-HOOK-CLOSE-STALE",
        "`close()` leaves stale hook metadata when hook is disabled",
        "new fd does not inherit old hook metadata", actual, "risk-check");
  }
  return risk::not_reproduced(
      "P0-HOOK-CLOSE-STALE",
      "`close()` leaves stale hook metadata when hook is disabled",
      "new fd does not inherit old hook metadata", actual, "risk-check");
}

risk::Result invalid_setsockopt_child() {
  int status = risk::run_child_with_timeout(
      []() {
        Coroutine *routine = co_create([]() {
          co_enable_hook_sys();
          int fd = socket(AF_INET, SOCK_STREAM, 0);
          risk::require_syscall(fd >= 0, "socket");
          setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, nullptr, 0);
          close(fd);
        });
        co_resume(routine);
        co_free(routine);
      },
      1000);

  std::string actual = risk::child_status_text(status);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 2) {
    return risk::needs_environment(
        "P1-SETSOCKOPT-INVALID",
        "invalid timeout `setsockopt()` arguments",
        "socket can be created", actual, "risk-check");
  }
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

struct BoundLocalPort {
  int fd{-1};
  int port{-1};
};

BoundLocalPort bind_local_port() {
  BoundLocalPort result;
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return result;
  }

  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return result;
  }
  socklen_t len = sizeof(addr);
  if (getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
    close(fd);
    return result;
  }

  result.fd = fd;
  result.port = ntohs(addr.sin_port);
  return result;
}

void connect_errno_child(int out_fd) {
  BoundLocalPort bound = bind_local_port();
  if (bound.fd < 0 || bound.port <= 0) {
    write_probe_line(out_fd, "bind local port failed: %s", strerror(errno));
    _exit(kProbeNeedsEnvironment);
  }

  BoolState state;
  run_bounded_coroutine(&state, [&state, port = bound.port]() {
    co_enable_hook_sys();
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      state.actual = std::string("socket failed: ") + strerror(errno);
      state.needs_environment = true;
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

  write_probe_line(out_fd, "%s", state.actual.c_str());
  close(bound.fd);
  if (state.needs_environment) {
    _exit(kProbeNeedsEnvironment);
  }
  _exit(state.confirmed ? kProbeConfirmed : kProbeNotReproduced);
}

risk::Result connect_errno_refused() {
  ChildProbeResult child =
      run_child_probe_with_timeout(connect_errno_child, 1500);
  std::string actual =
      child.output + "; " + risk::child_status_text(child.status);
  if (probe_exit_code(child.status, kProbeNeedsEnvironment)) {
    return risk::needs_environment(
        "P1-CONNECT-ERRNO", "hooked `connect()` errno behavior",
        "local TCP port can be bound", actual, "risk-check");
  }
  if (probe_exit_code(child.status, kProbeConfirmed) ||
      !risk::child_exited_cleanly(child.status)) {
    return risk::confirmed(
        "P1-CONNECT-ERRNO", "hooked `connect()` errno behavior",
        "connection refused is reported as ECONNREFUSED", actual,
        "risk-check");
  }
  return risk::not_reproduced(
      "P1-CONNECT-ERRNO", "hooked `connect()` errno behavior",
      "connection refused is reported as ECONNREFUSED", actual, "risk-check");
}

} // namespace

int main() {
  std::vector<risk::Result> results;
  results.push_back(stale_close_metadata());
  results.push_back(invalid_setsockopt_child());
  results.push_back(connect_errno_refused());
  return risk::summarize(results);
}
