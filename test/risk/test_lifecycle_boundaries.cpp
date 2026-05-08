#include "risk_common.h"
#include "co_async.h"
#include "co_future.h"
#include "co_routine.h"

#include <stdexcept>
#include <string>
#include <vector>

using namespace co;

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
  int status = risk::run_child_with_timeout(
      []() {
        Promise<int> promise;
        Future<int> future = promise.get_future();
        (void)future.get();
      },
      1000);
  std::string actual = risk::child_status_text(status);
  if (!risk::child_exited_cleanly(status)) {
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
};

static int abandoned_promise_loop(void *arg) {
  auto *state = static_cast<AbandonedPromiseState *>(arg);
  return state->done ? -1 : 0;
}

static risk::Result abandoned_promise_behavior() {
  int status = risk::run_child_with_timeout(
      []() {
        Future<int> *future = nullptr;
        {
          Promise<int> promise;
          future = new Future<int>(promise.get_future());
        }

        AbandonedPromiseState state;
        Coroutine *routine = co_create([future, &state]() {
          (void)future->get();
          state.done = true;
        });
        co_resume(routine);
        co_eventloop(abandoned_promise_loop, &state);

        co_free(routine);
        delete future;
      },
      1000);
  std::string actual = risk::child_status_text(status);
  if (!risk::child_exited_cleanly(status)) {
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
