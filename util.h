#pragma once

#include <cstddef>
#include <sys/time.h>

// container_of: given a pointer to a member, recover the containing object.
// Uses standard offsetof from <cstddef> instead of custom definition.
// Usage: container_of(ptr, Type, member_name)
#define container_of(ptr, type, member)                                        \
  reinterpret_cast<type *>(reinterpret_cast<char *>(ptr) -                     \
                           offsetof(type, member))

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
