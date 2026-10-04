#include "internal/io_backend.h"
#include "risk/risk_common.h"

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <dlfcn.h>
#include <sys/event.h>
#include <sys/socket.h>

static int fail_write_adds = 0;
static int fail_deletes = 0;
static int delete_error = EIO;

// Interpose only the syscall that can partially fail; keep the real kqueue.
extern "C" int kevent(int fd, const struct kevent *changes, int nchanges,
                      struct kevent *events, int nevents,
                      const struct timespec *timeout) {
  static auto real_kevent = reinterpret_cast<decltype(&kevent)>(
      dlsym(RTLD_NEXT, "kevent"));
  for (int i = 0; i < nchanges; ++i) {
    if ((changes[i].flags & EV_ADD) && changes[i].filter == EVFILT_WRITE &&
        fail_write_adds > 0) {
      --fail_write_adds;
      errno = ENOMEM;
      return -1;
    }
    if ((changes[i].flags & EV_DELETE) && fail_deletes > 0) {
      --fail_deletes;
      errno = delete_error;
      return -1;
    }
  }
  return real_kevent(fd, changes, nchanges, events, nevents, timeout);
}

static void require(bool condition, const char *message) {
  risk::require_syscall(condition, message);
}

static void failed_add(bool fail_cleanup) {
  int fds[2];
  require(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
  co::EpollCtx backend;
  int marker = 42;
  co::IoEvent event{POLLIN | POLLOUT, &marker};
  fail_write_adds = 1;
  fail_deletes = fail_cleanup ? 1 : 0;
  require(backend.add(fds[0], &event) == -1 && errno == ENOMEM,
          "add must preserve the registration error");
  require(write(fds[1], "x", 1) == 1, "write");
  struct kevent pending{};
  struct timespec zero{};
  if (!fail_cleanup) {
    require(kevent(backend.fd(), nullptr, 0, &pending, 1, &zero) == 0,
            "failed add must remove the successful kernel filter");
  }
  require(backend.wait(0) == 0, "failed add must not dispatch events");
  event.events = POLLIN;
  require(backend.add(fds[0], &event) == 0, "retry add after failure");
  require(backend.wait(0) == 1 && backend.event(0).data == &marker,
          "retry must deliver the new registration");
  require(backend.del(fds[0], &event) == 0, "delete");
  require(kevent(backend.fd(), nullptr, 0, &pending, 1, &zero) == 0,
          "retry must leave no stale kernel filters");
  close(fds[0]);
  close(fds[1]);
}

static void failed_mod() {
  int fds[2];
  require(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
  co::EpollCtx backend;
  int before = 1, after = 2;
  co::IoEvent event{POLLIN, &before};
  require(backend.add(fds[0], &event) == 0, "add");
  event = {POLLOUT, &after};
  fail_write_adds = 1;
  require(backend.mod(fds[0], &event) == -1 && errno == ENOMEM, "modify");
  require(write(fds[1], "x", 1) == 1, "write");
  require(backend.wait(0) == 1, "failed mod must restore old filter");
  require(backend.event(0).data == &before &&
          backend.event(0).events == POLLIN, "failed mod must preserve old data");
  require(backend.del(fds[0], &event) == 0, "delete");
  close(fds[0]);
  close(fds[1]);
}

static void failed_delete(int error) {
  int fds[2];
  require(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
  co::EpollCtx backend;
  int marker = 42;
  co::IoEvent event{POLLIN, &marker};
  require(backend.add(fds[0], &event) == 0, "add");
  fail_deletes = 1;
  delete_error = error;
  int result = backend.del(fds[0], &event);
  if (error == EINTR) {
    require(result == 0, "interrupted delete must retry");
  } else {
    require(result == -1 && errno == error, "delete must report failure");
  }
  require(write(fds[1], "x", 1) == 1, "write");
  require(backend.wait(0) == 0, "failed delete must not dispatch old data");
  require(backend.del(fds[0], &event) == 0, "retry delete");
  struct kevent pending{};
  struct timespec zero{};
  require(kevent(backend.fd(), nullptr, 0, &pending, 1, &zero) == 0,
          "delete must remove the kernel filter");
  close(fds[0]);
  close(fds[1]);
}

static void mod_closed_fd() {
  int fds[2];
  require(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair");
  co::EpollCtx backend;
  int marker = 42;
  co::IoEvent event{POLLIN, &marker};
  require(backend.add(fds[0], &event) == 0, "add");
  close(fds[0]);
  require(backend.mod(fds[0], &event) == -1 && errno == EBADF,
          "unchanged filters must still validate the fd");
  require(backend.del(fds[0], &event) == 0, "delete closed fd");
  close(fds[1]);
}
#endif

int main() {
#if defined(__APPLE__) || defined(__FreeBSD__)
  struct Case { const char *name; void (*run)(); } cases[] = {
    {"partial add rollback", [] { failed_add(false); }},
    {"partial add with failed rollback", [] { failed_add(true); }},
    {"failed mod preserves old registration", failed_mod},
    {"failed delete suppresses stale events", [] { failed_delete(EIO); }},
    {"interrupted delete retries", [] { failed_delete(EINTR); }},
    {"mod validates a closed fd", mod_closed_fd},
  };
  int failures = 0;
  for (const auto &c : cases) {
    int status = risk::run_child_with_timeout(c.run, 2000);
    bool ok = risk::child_exited_cleanly(status);
    printf("%s: %s (%s)\n", ok ? "PASS" : "FAIL", c.name,
           risk::child_status_text(status).c_str());
    failures += !ok;
  }
  return failures ? 1 : 0;
#else
  printf("SKIP: kqueue rollback requires macOS or FreeBSD\n");
  return 0;
#endif
}
