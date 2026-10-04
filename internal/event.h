#pragma once
#include "wait.h"
#include "timer_queue.h"
#include <cstdint>

namespace co {
namespace detail {
// I/O-specific completion adapter. The backend treats this as an opaque payload.
struct IoSource {
  void (*notify)(IoSource*, uint32_t);
};
class WaitTimer : public TimerItem {
public:
  explicit WaitTimer(WaitRecord& waiter) : waiter_(waiter) {}
  ~WaitTimer() { Cancel(); }
  int Arm(unsigned long long deadline);
  void Cancel() noexcept { Timeout::Remove(this); }
private:
  WaitRecord& waiter_;
  friend class co::ThreadEnv;
};
}
}
