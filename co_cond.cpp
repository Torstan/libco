#include "co_cond.h"
#include "internal/event.h"
#include "internal/util.h"
#include <cerrno>
#include <exception>
#include <stdexcept>

namespace co {
struct CoCondItem : LinkItemBase<CoCondItem> {
  detail::WaitRecord wait;
  detail::WaitTimer timer{wait};
  ~CoCondItem() {
    LinkedList<CoCondItem>::remove(this);
    timer.Cancel();
    wait.SetCleanup(nullptr, nullptr);
  }
  static void Detach(void *arg) noexcept {
    auto* item = static_cast<CoCondItem*>(arg);
    LinkedList<CoCondItem>::remove(item);
    item->timer.Cancel();
  }
};
struct CoCond::Impl { LinkedList<CoCondItem> waiters; };
CoCond::CoCond() : impl_(std::make_unique<Impl>()) {}
CoCond::~CoCond() {
  if (!impl_->waiters.empty()) std::terminate();
}
int CoCond::Signal() {
  auto* waiter = impl_->waiters.head;
  if (waiter) {
    waiter->wait.Complete();
    impl_->waiters.pop_head();
  }
  return 0;
}
int CoCond::Broadcast() {
  while (!impl_->waiters.empty()) Signal();
  return 0;
}
int CoCond::Timedwait(int ms) {
  try {
    CoCondItem item;
    if (ms > 0) {
      int ret = item.timer.Arm(GetTickMS() + ms);
      if (ret) return ret;
    }
    item.wait.SetCleanup(CoCondItem::Detach, &item);
    impl_->waiters.add_tail(&item);
    item.wait.Suspend();
    return 0;
  } catch (const std::logic_error&) {
    errno = EINVAL;
    return -1;
  }
}
} // namespace co
