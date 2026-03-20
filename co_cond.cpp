#include "co_cond.h"
#include "co_epoll.h"
#include "co_link.h"
#include "co_routine.h"
#include <stdlib.h>

namespace co {

static void OnSignalProcessEvent(TimeoutItem *item) {
  Coroutine *co = (Coroutine *)item->arg;
  co_resume(co);
}

int CoCond::Signal() {
  CoCondItem *cond_item = Pop();
  if (!cond_item) {
    return 0;
  }
  TimeoutItemLink::remove(&cond_item->timeout);

  co_get_curr_thread_env()->Epoll()->active_list()->add_tail(
      &cond_item->timeout);

  return 0;
}
int CoCond::Broadcast() {
  for (;;) {
    CoCondItem *cond_item = Pop();
    if (!cond_item)
      return 0;

    TimeoutItemLink::remove(&cond_item->timeout);

    co_get_curr_thread_env()->Epoll()->active_list()->add_tail(
        &cond_item->timeout);
  }

  return 0;
}
int CoCond::Timedwait(int ms) {
  CoCondItem *cond_item = (CoCondItem *)calloc(1, sizeof(CoCondItem));
  cond_item->timeout.arg = co_self();
  cond_item->timeout.process_func = OnSignalProcessEvent;

  if (ms > 0) {
    unsigned long long now = GetTickMS();
    cond_item->timeout.expire_time_ms = now + ms;

    int ret = co_get_curr_thread_env()->Epoll()->timeout()->AddItem(
        &cond_item->timeout, now);
    if (ret != 0) {
      free(cond_item);
      return ret;
    }
  }

  add_tail(cond_item);

  co_yield_ct();

  remove(cond_item);
  free(cond_item);

  return 0;
}
CoCondItem *CoCond::Pop() { return pop_head(); }

} // namespace co
