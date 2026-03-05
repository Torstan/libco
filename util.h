#pragma once

#include "sys/time.h"

#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((size_t) & ((TYPE *)0)->MEMBER)
#endif

#ifndef container_of
#define container_of(ptr, type, member)                                        \
  ({                                                                           \
    const decltype(((type *)0)->member) *__mptr = (ptr);                       \
    (type *)((char *)__mptr - offsetof(type, member));                         \
  })
#endif

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
