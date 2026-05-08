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
    printf("scenario: multi-thread hook fd table access\n");
    printf("expected: socket can be created for hook metadata race probe\n");
    printf("actual: failed to create hooked socket\n");
    printf("status: needs environment\n");
    printf("regression: risk-diagnose only\n");
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
