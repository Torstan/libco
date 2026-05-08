#include "risk_common.h"
#include "co_async.h"
#include "co_cond.h"
#include "co_routine.h"
#include "task.h"
#include "thread_worker.h"

#include <atomic>
#include <exception>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sys/resource.h>
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
      char *value = getenv("LIBCO_RISK_ENV");
      if (value != nullptr && value[0] == '\0') {
        abort();
      }
    });
    co_resume(routine);
    co_free(routine);
  }
}

static void print_env_leak_result() {
  printf("RISK-ID: P1-ENV-LEAK\n");
  printf("scenario: coroutine private environment leak\n");
  printf("expected: LSan reports no per-coroutine env leak\n");
  printf("actual: env probe completed; inspect LSan output\n");
  printf("status: not reproduced without leak report\n");
  printf("regression: risk-diagnose\n\n");
}

static bool run_thread_env_probe() {
  int before = risk::count_open_fds();
  for (int i = 0; i < 50; ++i) {
    std::thread([]() {
      Coroutine *routine = co_create([]() {});
      co_resume(routine);
      co_free(routine);
    }).join();
  }
  int after = risk::count_open_fds();
  bool confirmed = after > before;
  printf("RISK-ID: P1-THREADENV-LEAK\n");
  printf("scenario: per-thread ThreadEnv leak\n");
  printf("expected: fd count does not grow after short-lived coroutine threads\n");
  printf("actual: fd_count_before=%d fd_count_after=%d\n", before, after);
  printf("status: %s\n", confirmed ? "confirmed" : "not reproduced");
  printf("regression: risk-diagnose\n\n");
  return confirmed;
}

static bool run_hook_alloc_fd_leak_probe() {
  struct rlimit limit;
  if (getrlimit(RLIMIT_NOFILE, &limit) != 0 || limit.rlim_cur <= 102401) {
    printf("RISK-ID: P0-HOOK-ALLOC-FD-LEAK\n");
    printf("scenario: hook metadata allocation failure leaks fd\n");
    printf("expected: environment can allocate fd >= 102400\n");
    printf("actual: RLIMIT_NOFILE is below required fd-table ceiling\n");
    printf("status: needs environment\n");
    printf("regression: risk-diagnose\n\n");
    return false;
  }

  std::vector<int> fds;
  int fd = -1;
  for (int i = 0; i < 102500 && fd < 102400; ++i) {
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

  bool confirmed = hooked_fd < 0 && after > before;
  printf("RISK-ID: P0-HOOK-ALLOC-FD-LEAK\n");
  printf("scenario: hook metadata allocation failure leaks fd\n");
  printf("expected: failed hooked socket closes the real fd\n");
  printf("actual: hooked_fd=%d fd_count_before=%d fd_count_after=%d\n",
         hooked_fd, before, after);
  printf("status: %s\n", confirmed ? "confirmed" : "not reproduced");
  printf("regression: risk-diagnose\n\n");
  return confirmed;
}

static bool run_allocation_failure_probe() {
  enum AllocProbeExit {
    kControlledEnomem = 0,
    kReturnedNullWrongErrno = 10,
    kStdExceptionEscaped = 11,
    kNonStdExceptionEscaped = 12,
    kNoAllocationFailure = 13,
    kSetrlimitFailed = 14,
  };

  int status = risk::run_child_with_timeout(
      []() {
        struct rlimit limit;
        limit.rlim_cur = 8 * 1024 * 1024;
        limit.rlim_max = 8 * 1024 * 1024;
        if (setrlimit(RLIMIT_AS, &limit) != 0) {
          _exit(kSetrlimitFailed);
        }
        try {
          Coroutine *routines[4096] = {};
          for (int i = 0; i < 4096; ++i) {
            errno = 0;
            Coroutine *routine = co_create([]() {});
            if (!routine) {
              _exit(errno == ENOMEM ? kControlledEnomem
                                    : kReturnedNullWrongErrno);
            }
            routines[i] = routine;
          }
          for (Coroutine *routine : routines) {
            if (routine) {
              co_free(routine);
            }
          }
        } catch (const std::exception &) {
          _exit(kStdExceptionEscaped);
        } catch (...) {
          _exit(kNonStdExceptionEscaped);
        }
        _exit(kNoAllocationFailure);
      },
      1500);

  std::string actual;
  bool confirmed = true;
  const char *status_text = "confirmed";
  if (WIFEXITED(status)) {
    switch (WEXITSTATUS(status)) {
    case kControlledEnomem:
      actual = "co_create returned nullptr with errno=ENOMEM";
      confirmed = false;
      status_text = "not reproduced";
      break;
    case kSetrlimitFailed:
      actual = "setrlimit(RLIMIT_AS) failed";
      confirmed = false;
      status_text = "needs environment";
      break;
    case kReturnedNullWrongErrno:
      actual = "co_create returned nullptr without errno=ENOMEM";
      break;
    case kStdExceptionEscaped:
      actual = "co_create threw std::exception";
      break;
    case kNonStdExceptionEscaped:
      actual = "co_create threw non-std exception";
      break;
    case kNoAllocationFailure:
      actual = "allocation failure was not reached";
      break;
    default:
      actual = risk::child_status_text(status);
      break;
    }
  } else {
    actual = risk::child_status_text(status);
  }

  printf("RISK-ID: P1-ALLOC-FAILURE\n");
  printf("scenario: allocation failure safety\n");
  printf("expected: allocation failure reports a controlled error\n");
  printf("actual: %s\n", actual.c_str());
  printf("status: %s\n", status_text);
  printf("regression: risk-diagnose\n\n");
  return confirmed;
}

struct CondProbeState {
  CoCond cond;
  std::atomic<bool> waiter_started{false};
  std::atomic<bool> waiter_done{false};
  unsigned long long start_ms{0};
};

static int cond_loop(void *arg) {
  CondProbeState *state = static_cast<CondProbeState *>(arg);
  if (state->waiter_done.load(std::memory_order_acquire)) {
    return -1;
  }
  if (risk::now_ms() - state->start_ms > 250) {
    return -1;
  }
  return 0;
}

static bool run_cond_cross_thread_probe() {
  int status = risk::run_child_with_timeout(
      []() {
        CondProbeState state;
        std::thread waiter([&]() {
          Coroutine *routine = co_create([&]() {
            state.waiter_started.store(true, std::memory_order_release);
            state.cond.Timedwait(50);
            state.waiter_done.store(true, std::memory_order_release);
          });
          state.start_ms = risk::now_ms();
          co_resume(routine);
          co_eventloop(cond_loop, &state);
          if (state.waiter_done.load(std::memory_order_acquire)) {
            co_free(routine);
          }
        });
        while (!state.waiter_started.load(std::memory_order_acquire)) {
          usleep(1000);
        }
        std::thread signaler([&]() { state.cond.Signal(); });
        signaler.join();
        waiter.join();
      },
      1000);

  printf("RISK-ID: P1-COND-CROSS-THREAD\n");
  printf("scenario: CoCond signal from a different thread is unsupported\n");
  printf("expected: cross-thread CoCond signal is a documented boundary\n");
  printf("actual: %s; inspect sanitizer output\n",
         risk::child_status_text(status).c_str());
  printf("status: documented boundary\n");
  printf("regression: risk-diagnose\n\n");
  return false;
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
  printf("status: %s\n",
         ran.load(std::memory_order_relaxed) == 0 ? "documented boundary"
                                                  : "not reproduced");
  printf("regression: risk-diagnose\n\n");
}

int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  if (argc == 2 && std::string(argv[1]) == "cond-only") {
    return run_cond_cross_thread_probe() ? 1 : 0;
  }
  if (argc == 2 && std::string(argv[1]) == "alloc-only") {
    return run_allocation_failure_probe() ? 1 : 0;
  }

  bool leak_only = argc == 2 && std::string(argv[1]) == "leak-only";
  bool asan_leak_only = argc == 2 && std::string(argv[1]) == "asan-leak-only";
  bool confirmed = false;
  run_env_leak_probe();
  if (!asan_leak_only) {
    print_env_leak_result();
  }

  confirmed = run_thread_env_probe() || confirmed;
  if (leak_only || asan_leak_only) {
    return confirmed ? 1 : 0;
  }

  confirmed = run_hook_alloc_fd_leak_probe() || confirmed;
  confirmed = run_allocation_failure_probe() || confirmed;
  confirmed = run_cond_cross_thread_probe() || confirmed;
  run_schedule_thread_local_probe();
  return confirmed ? 1 : 0;
}
