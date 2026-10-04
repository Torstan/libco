#pragma once

#include "co_link.h"
#include "util.h"

namespace co {

struct TimerItem : LinkItemBase<TimerItem> {
  unsigned long long expire_time_ms{0};
};
using TimerList = LinkedList<TimerItem>;


class Timeout {
  static constexpr int item_size = 60 * 1000;
  TimerList items[item_size];
  unsigned long long start_time_ms{GetTickMS()};
  long long start_idx{0};

public:
  int AddItem(TimerItem *item, unsigned long long now_ms) {
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
  // Removing an unregistered item is a no-op.
  static void Remove(TimerItem *item) noexcept { TimerList::remove(item); }

  // Append only items whose deadline is <= now_ms; retain later deadlines.
  void TakeAll(unsigned long long now_ms, TimerList *result) {
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
    TimerList pending;
    for (int i = 0; i < cnt; i++) {
      int idx = (start_idx + i) % item_size;
      pending.join(items[idx]);
    }
    start_time_ms = now_ms;
    start_idx += cnt - 1;
    // Reinsert only after scanning and advancing, so no item is scanned twice.
    while (auto *item = pending.pop_head()) {
      if (item->expire_time_ms <= now_ms) {
        result->add_tail(item);
      } else {
        AddItem(item, now_ms);
      }
    }
  }
};

} // namespace co
