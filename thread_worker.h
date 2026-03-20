#pragma once
#include "routine_context.h"

namespace co {

class ThreadWorker {
public:
  explicit ThreadWorker(int idx);
  ~ThreadWorker() {}
  void run_loop(bool forever = true);
  static thread_local RoutineContext *current_context;
  static void switch_in(RoutineContext* ctx);
  static void switch_out(RoutineContext* ctx);
private:
  int thread_idx;
};

} // namespace co
