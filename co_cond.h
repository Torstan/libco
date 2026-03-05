#pragma once
#include "co_link.h"
#include "co_timeout.h"

class CoCond;
struct CoCondItem : public LinkItemBase<CoCondItem> {
  TimeoutItem timeout;
};

class CoCond : public LinkedList<CoCondItem> {
public:
  CoCond() = default;
  ~CoCond() = default;

  int Signal();
  int Broadcast();
  int Timedwait(int timeout_ms);

private:
  CoCondItem *Pop();
};
