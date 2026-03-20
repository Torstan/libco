#pragma once

#include "co_link.h"
#include "util.h"
#include <sys/epoll.h>

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

class Timeout {
  static constexpr int item_size = 60 * 1000;
  TimeoutItemLink items[item_size];
  unsigned long long start_time_ms{GetTickMS()};
  long long start_idx{0};

public:
  int AddItem(TimeoutItem *item, unsigned long long now_ms) {
    if (start_time_ms == 0) {
      start_time_ms = now_ms;
      start_idx = 0;
    }
    if (now_ms < start_time_ms) {
      co_log_err("CO_ERR: AddItem line %d now_ms %llu start_time_ms %llu",
                 __LINE__, now_ms, start_time_ms);

      return __LINE__;
    }
    if (item->expire_time_ms < now_ms) {
      co_log_err("CO_ERR: AddItem line %d item->expire_time_ms %llu now_ms "
                 "%llu start_time_ms %llu",
                 __LINE__, item->expire_time_ms, now_ms, start_time_ms);

      return __LINE__;
    }
    unsigned long long diff = item->expire_time_ms - start_time_ms;

    if (diff >= (unsigned long long)item_size) {
      diff = item_size - 1;
      co_log_err("CO_ERR: AddItem line %d diff %d", __LINE__, diff);

      // return __LINE__;
    }
    items[(start_idx + diff) % item_size].add_tail(item);

    return 0;
  }
  void TakeAll(unsigned long long now_ms, TimeoutItemLink *result) {
    if (start_time_ms == 0) {
      start_time_ms = now_ms;
      start_idx = 0;
    }

    if (now_ms < start_time_ms) {
      return;
    }
    int cnt = now_ms - start_time_ms + 1;
    if (cnt > item_size) {
      cnt = item_size;
    }
    if (cnt < 0) {
      return;
    }
    for (int i = 0; i < cnt; i++) {
      int idx = (start_idx + i) % item_size;
      result->join(items[idx]);
    }
    start_time_ms = now_ms;
    start_idx += cnt - 1;
  }
};

} // namespace co