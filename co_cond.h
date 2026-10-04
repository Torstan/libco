#pragma once
#include <memory>

namespace co {
// Single-thread condition waiters. The condition must outlive its waiters.
class CoCond {
public:
  CoCond();
  ~CoCond();
  CoCond(const CoCond&) = delete;
  CoCond& operator=(const CoCond&) = delete;
  int Signal();
  int Broadcast();
  int Timedwait(int timeout_ms);
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
