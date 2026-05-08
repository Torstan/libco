#pragma once

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <functional>
#include <string>
#include <vector>

namespace risk {

enum class Status {
  kPassed,
  kConfirmed,
  kNotReproduced,
  kNeedsEnvironment,
  kDocumentedBoundary,
};

struct Result {
  const char *id;
  const char *scenario;
  const char *expected;
  std::string actual;
  Status status;
  const char *regression;
};

inline const char *status_name(Status status) {
  switch (status) {
  case Status::kPassed:
    return "passed";
  case Status::kConfirmed:
    return "confirmed";
  case Status::kNotReproduced:
    return "not reproduced";
  case Status::kNeedsEnvironment:
    return "needs environment";
  case Status::kDocumentedBoundary:
    return "documented boundary";
  }
  return "unknown";
}

inline Result passed(const char *id, const char *scenario,
                     const char *expected, const std::string &actual,
                     const char *regression) {
  return Result{id, scenario, expected, actual, Status::kPassed, regression};
}

inline Result confirmed(const char *id, const char *scenario,
                        const char *expected, const std::string &actual,
                        const char *regression) {
  return Result{id, scenario, expected, actual, Status::kConfirmed,
                regression};
}

inline Result not_reproduced(const char *id, const char *scenario,
                             const char *expected, const std::string &actual,
                             const char *regression) {
  return Result{id, scenario, expected, actual, Status::kNotReproduced,
                regression};
}

inline Result needs_environment(const char *id, const char *scenario,
                                const char *expected,
                                const std::string &actual,
                                const char *regression) {
  return Result{id, scenario, expected, actual, Status::kNeedsEnvironment,
                regression};
}

inline Result documented_boundary(const char *id, const char *scenario,
                                  const char *expected,
                                  const std::string &actual,
                                  const char *regression) {
  return Result{id, scenario, expected, actual, Status::kDocumentedBoundary,
                regression};
}

inline void print_result(const Result &result) {
  printf("RISK-ID: %s\n", result.id);
  printf("scenario: %s\n", result.scenario);
  printf("expected: %s\n", result.expected);
  printf("actual: %s\n", result.actual.c_str());
  printf("status: %s\n", status_name(result.status));
  printf("regression: %s\n\n", result.regression);
}

inline int summarize(const std::vector<Result> &results) {
  int confirmed_count = 0;
  for (const Result &result : results) {
    print_result(result);
    if (result.status == Status::kConfirmed) {
      ++confirmed_count;
    }
  }
  return confirmed_count == 0 ? 0 : 1;
}

inline int count_open_fds() {
  DIR *dir = opendir("/proc/self/fd");
  if (!dir) {
    return -1;
  }
  int count = 0;
  while (readdir(dir)) {
    ++count;
  }
  closedir(dir);
  return count;
}

inline std::string child_status_text(int status) {
  char buf[128];
  if (WIFEXITED(status)) {
    snprintf(buf, sizeof(buf), "child exited with status %d",
             WEXITSTATUS(status));
  } else if (WIFSIGNALED(status)) {
    snprintf(buf, sizeof(buf), "child terminated by signal %d",
             WTERMSIG(status));
  } else {
    snprintf(buf, sizeof(buf), "child ended with raw status %d", status);
  }
  return std::string(buf);
}

inline int run_child(const std::function<void()> &fn) {
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return 255;
  }
  if (pid == 0) {
    fn();
    _exit(0);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      perror("waitpid");
      return 255;
    }
  }
  return status;
}

inline bool child_exited_cleanly(int status) {
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

inline void require_syscall(bool condition, const char *message) {
  if (!condition) {
    perror(message);
    _exit(2);
  }
}

inline void set_cpu_seconds_limit(int seconds) {
  struct rlimit limit;
  limit.rlim_cur = seconds;
  limit.rlim_max = seconds;
  setrlimit(RLIMIT_CPU, &limit);
}

} // namespace risk
