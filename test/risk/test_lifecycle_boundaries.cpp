#include "risk_common.h"
#include "co_async.h"
#include "co_future.h"
#include "co_routine.h"

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace co;

namespace {

enum AbandonedPromiseProbeExit {
  kAbandonedPromiseSafe = 0,
  kAbandonedPromiseInvalidState = 1,
};

enum FutureNoContextProbeExit {
  kFutureNoContextSafe = 0,
  kFutureNoContextReturned = 1,
  kFutureNoContextUnclearException = 2,
  kFutureNoContextNonStdException = 3,
};

struct ChildProbeResult {
  int status{255};
  std::string output;
};

void write_probe_actual(int fd, const std::string &actual) {
  ssize_t ignored = write(fd, actual.c_str(), actual.size());
  (void)ignored;
}

ChildProbeResult run_child_probe_with_timeout(
    const std::function<int(int)> &fn, int timeout_ms) {
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
    int code = fn(pipefd[1]);
    close(pipefd[1]);
    _exit(code);
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

  if (output.empty()) {
    output = risk::child_status_text(status);
  }
  return ChildProbeResult{status, output};
}

} // namespace

static risk::Result resume_ended_coroutine() {
  int status = risk::run_child_with_timeout(
      []() {
        Coroutine *routine = co_create([]() {});
        co_resume(routine);
        co_resume(routine);
        co_free(routine);
      },
      1000);
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
  ChildProbeResult child = run_child_probe_with_timeout(
      [](int out_fd) {
        Promise<int> promise;
        Future<int> future = promise.get_future();
        try {
          int value = future.get();
          write_probe_actual(out_fd, "invalid state: future.get() returned " +
                                         std::to_string(value));
          return kFutureNoContextReturned;
        } catch (const std::exception &ex) {
          std::string actual = std::string("caught exception: ") + ex.what();
          write_probe_actual(out_fd, actual);
          return std::string(ex.what()).empty()
                     ? kFutureNoContextUnclearException
                     : kFutureNoContextSafe;
        } catch (...) {
          write_probe_actual(out_fd, "caught non-std exception");
          return kFutureNoContextNonStdException;
        }
      },
      1000);
  std::string actual = child.output;
  if (!risk::child_exited_cleanly(child.status)) {
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

struct AbandonedPromiseState {
  bool done{false};
  bool invalid_state{false};
  std::string actual;
};

static int abandoned_promise_loop(void *arg) {
  auto *state = static_cast<AbandonedPromiseState *>(arg);
  return state->done ? -1 : 0;
}

static risk::Result abandoned_promise_behavior() {
  ChildProbeResult child = run_child_probe_with_timeout(
      [](int out_fd) {
        Future<int> *future = nullptr;
        {
          Promise<int> promise;
          future = new Future<int>(promise.get_future());
        }

        AbandonedPromiseState state;
        Coroutine *routine = co_create([future, &state]() {
          try {
            int value = future->get();
            state.actual =
                "invalid state: future->get() returned " +
                std::to_string(value);
            state.invalid_state = true;
          } catch (const std::exception &ex) {
            state.actual = std::string("caught exception: ") + ex.what();
          } catch (...) {
            state.actual = "caught non-std exception";
          }
          state.done = true;
        });
        co_resume(routine);
        co_eventloop(abandoned_promise_loop, &state);

        write_probe_actual(out_fd, state.actual);
        co_free(routine);
        delete future;
        return state.invalid_state ? kAbandonedPromiseInvalidState
                                   : kAbandonedPromiseSafe;
      },
      1000);
  std::string actual = child.output;
  if (!risk::child_exited_cleanly(child.status)) {
    return risk::confirmed(
        "P1-PROMISE-ABANDONED", "abandoned promise behavior",
        "future observes a clear broken-promise result or bounded wait without abort",
        actual, "risk-check");
  }
  return risk::not_reproduced(
      "P1-PROMISE-ABANDONED", "abandoned promise behavior",
      "future observes a clear broken-promise result or bounded wait without abort",
      actual, "risk-check");
}

static risk::Result coroutine_throw_boundary() {
  int status = risk::run_child_with_timeout(
      []() {
        Coroutine *routine =
            co_create([]() { throw std::runtime_error("boom"); });
        co_resume(routine);
        co_free(routine);
      },
      1000);
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
