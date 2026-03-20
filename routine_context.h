#pragma once
#include "coctx.h"
#include <ucontext.h>

namespace co {

class RoutineContext {
public:
  explicit RoutineContext();
  ~RoutineContext() {}
  void InitCtx(char* stack_buf, size_t stack_size);
  void MakeCtx(coctx_func_t func, void *arg1);
  void switch_in();
  void switch_out();

private:
#ifdef USE_UCONTEXT
  ucontext_t uctx;
#else
  coctx_t ctx;
#endif
  RoutineContext *prev_link;
  RoutineContext *next_link;
};

} // namespace co