#include "co_routine.h"
#include "internal/poll.h"
#include "risk/risk_common.h"
#include <arpa/inet.h>
#include <sys/socket.h>

struct Case {
  const char *name;
  short revents;
  int poll_result;
  int poll_error;
  bool listening;
  int expected_error;
};
static const Case *current_case = nullptr;

// Replace the existing wait boundary only in this executable. Connections and
// SO_ERROR stay real; error-only readiness must not depend on kernel timing.
int co::detail::PollWait(pollfd *fds, nfds_t nfds, int, PollFunc native_poll) {
  risk::require_syscall(nfds == 1 && native_poll, "connect poll arguments");
  if (current_case->poll_result <= 0) {
    fds[0].revents = 0;
    errno = current_case->poll_error;
    return current_case->poll_result;
  }
  risk::require_syscall(native_poll(fds, nfds, 1000) == 1,
                        "native connection completion");
  fds[0].revents = current_case->revents;
  return 1;
}

static void run_case(const Case &c) {
  current_case = &c;
  int listener = socket(AF_INET, SOCK_STREAM, 0);
  risk::require_syscall(listener >= 0, "socket");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  risk::require_syscall(bind(listener, reinterpret_cast<sockaddr*>(&address),
                            sizeof(address)) == 0, "bind");
  socklen_t length = sizeof(address);
  risk::require_syscall(getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                                   &length) == 0, "getsockname");
  if (c.listening) risk::require_syscall(listen(listener, 1) == 0, "listen");
  else close(listener);

  bool done = false;
  auto *coroutine = co::co_create([&] {
    co::co_enable_hook_sys();
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    risk::require_syscall(fd >= 0, "hooked socket");
    int result = connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    int error = errno;
    close(fd);
    if (result != (c.expected_error ? -1 : 0) ||
        (c.expected_error && error != c.expected_error)) {
      fprintf(stderr, "connect result=%d errno=%d; expected errno=%d\n",
              result, error, c.expected_error);
      _exit(1);
    }
    done = true;
  });
  co::co_resume(coroutine);
  risk::require_syscall(done, "connect must return");
  co::co_free(coroutine);
  if (c.listening) close(listener);
}

int main() {
  const Case cases[] = {
    {"refused with POLLHUP only", POLLHUP, 1, 0, false, ECONNREFUSED},
    {"refused with POLLERR only", POLLERR, 1, 0, false, ECONNREFUSED},
    {"refused with POLLOUT", POLLOUT, 1, 0, false, ECONNREFUSED},
    {"successful connection", POLLOUT, 1, 0, true, 0},
    {"poll error is preserved", 0, -1, EINVAL, false, EINVAL},
    {"interrupted retries preserve EINTR", 0, -1, EINTR, false, EINTR},
    {"poll timeout reports ETIMEDOUT", 0, 0, 0, false, ETIMEDOUT},
  };
  int failures = 0;
  for (const auto &c : cases) {
    int status = risk::run_child_with_timeout([&] { run_case(c); }, 2000);
    bool ok = risk::child_exited_cleanly(status);
    printf("%s: %s (%s)\n", ok ? "PASS" : "FAIL", c.name,
           risk::child_status_text(status).c_str());
    failures += !ok;
  }
  return failures ? 1 : 0;
}
