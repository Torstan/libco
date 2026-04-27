#include "co_routine.h"

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

using namespace co;

static bool done = false;
static int failures = 0;

static void expect(bool condition, const char *message) {
  if (!condition) {
    fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
  }
}

static void test_duplicate_poll_entries() {
  int pipefd[2];
  expect(pipe(pipefd) == 0, "pipe should succeed");
  if (failures) {
    done = true;
    return;
  }

  const char byte = 'x';
  expect(write(pipefd[1], &byte, 1) == 1, "write should make pipe readable");

  struct pollfd fds[2] = {{pipefd[0], POLLIN, 0}, {pipefd[0], POLLIN, 0}};
  co_enable_hook_sys();
  int ret = poll(fds, 2, 1000);

  expect(ret == 2, "poll should count each ready duplicate entry");
  expect((fds[0].revents & POLLIN) != 0, "first duplicate should be readable");
  expect((fds[1].revents & POLLIN) != 0, "second duplicate should be readable");

  close(pipefd[0]);
  close(pipefd[1]);
  done = true;
}

static int loop(void *) { return done ? -1 : 0; }

int main() {
  Coroutine *co = co_create(test_duplicate_poll_entries);
  co_resume(co);
  co_eventloop(loop, nullptr);
  co_free(co);
  return failures == 0 ? 0 : 1;
}
