#pragma once

#include "io_backend.h"
#include "co_link.h"

namespace co {

struct TimeoutItem;
struct TimeoutItemLink;

typedef void (*prepare_func_t)(TimeoutItem *, struct epoll_event &ev,
                               TimeoutItemLink *active);
typedef void (*process_func_t)(TimeoutItem *);

struct TimeoutItem : public LinkItemBase<TimeoutItem> {
  unsigned long long expire_time_ms;

  prepare_func_t prepare_func;
  process_func_t process_func;

  void *arg; // routine
  bool timeout;
};

struct TimeoutItemLink : LinkedList<TimeoutItem> {};

} // namespace co
