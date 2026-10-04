#pragma once
#include "co_link.h"

namespace co {
class Coroutine;
class ThreadEnv;
namespace detail {
Coroutine& RequireWaiter();

// One thread-local suspension. Sources remain alive until Detach runs; completion
// only queues the waiter, so a whole batch of I/O results can be collected first.
class WaitRecord : private LinkItemBase<WaitRecord> {
public:
  WaitRecord();
  ~WaitRecord();
  WaitRecord(const WaitRecord&) = delete;
  WaitRecord& operator=(const WaitRecord&) = delete;
  void Suspend();
  void Complete();
  void SetCleanup(void (*cleanup)(void*) noexcept, void *arg);
private:
  void Detach() noexcept;
  Coroutine *coroutine_;
  ThreadEnv *owner_;
  bool completed_{false};
  bool suspended_{false};
  void (*cleanup_)(void*) noexcept{nullptr};
  void *arg_{nullptr};
  friend class co::LinkedList<WaitRecord>;
  friend class co::ThreadEnv;
  friend class WaitTimer;
};
}
}
