#pragma once

#include <sys/time.h>

namespace co {

inline void co_log_err(const char *fmt, ...) {}

inline unsigned long long GetTickUS() {
  struct timeval now = {0};
  gettimeofday(&now, nullptr);
  unsigned long long u = now.tv_sec;
  u *= 1000000;
  u += now.tv_usec;
  return u;
}
inline unsigned long long GetTickMS() { return GetTickUS() / 1000; }

} // namespace co
