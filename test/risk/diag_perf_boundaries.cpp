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

static bool run_idle_cpu_probe() {
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
  bool confirmed = (after_us - before_us) > 50000;
  printf("RISK-ID: P2-IDLE-CPU\n");
  printf("scenario: empty event loop idle CPU\n");
  printf("expected: idle loop consumes little CPU while waiting\n");
  printf("actual: cpu_us=%ld duration_ms=%d\n", after_us - before_us,
         state.duration_ms);
  printf("status: %s\n", confirmed ? "confirmed" : "not reproduced");
  printf("regression: risk-diagnose\n\n");
  return confirmed;
}

static bool run_run_loop_cleanup_probe() {
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

  bool confirmed = after_second > after_first;
  printf("RISK-ID: P2-RUN-LOOP-CLEANUP\n");
  printf("scenario: run_loop(false) final coroutine cleanup\n");
  printf("expected: completed task coroutine is cleaned in the same run\n");
  printf("actual: destructed_after_first=%d destructed_after_second=%d\n",
         after_first, after_second);
  printf("status: %s\n", confirmed ? "confirmed" : "not reproduced");
  printf("regression: risk-diagnose\n\n");
  return confirmed;
}

int main() {
  bool confirmed = false;
  confirmed = run_idle_cpu_probe() || confirmed;
  confirmed = run_run_loop_cleanup_probe() || confirmed;
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
  return confirmed ? 1 : 0;
}
