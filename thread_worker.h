#pragma once
#include "routine_context.h"

class ThreadWorker {
public:
  explicit ThreadWorker(int idx);
  ~ThreadWorker() {}
  void RunLoop();
  static thread_local RoutineContext *current_context;

private:
  int thread_idx;
};
