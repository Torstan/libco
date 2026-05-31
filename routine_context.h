#pragma once
#include "coctx.h"

#ifdef USE_UCONTEXT
#include <stdint.h>
#include <ucontext.h>
#endif

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
  static void Entry(uint32_t low, uint32_t high);
  coctx_func_t func_{nullptr};
  void *arg_{nullptr};
  ucontext_t uctx;
#else
  coctx_t ctx;
#endif
  RoutineContext *prev_link;
  RoutineContext *next_link;
};

} // namespace co
