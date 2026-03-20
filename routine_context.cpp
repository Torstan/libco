#include "routine_context.h"
#include "thread_worker.h"
#include <assert.h>

namespace co {

extern "C" {
extern void coctx_swap(coctx_t *, coctx_t *) asm("coctx_swap");
};

typedef void (*ucontext_func_t) (void);

RoutineContext::RoutineContext() : prev_link(nullptr), next_link(nullptr) {
#ifndef USE_UCONTEXT
  coctx_init(&ctx);
#endif
}
void RoutineContext::InitCtx(char* stack_buf, size_t stack_size) {
#ifdef USE_UCONTEXT
  getcontext(&uctx);
  uctx.uc_stack.ss_sp = stack_buf;
  uctx.uc_stack.ss_size = stack_size;
  uctx.uc_link = nullptr;
#else
  ctx.ss_sp = stack_buf;
  ctx.ss_size = stack_size;
#endif
}

void RoutineContext::MakeCtx(coctx_func_t func, void *arg1) {
#ifdef USE_UCONTEXT
  makecontext(&uctx, (ucontext_func_t)func, 2, arg1, nullptr);
#else
  coctx_make(&ctx, func, arg1, nullptr);
#endif
}

void RoutineContext::switch_in() {
  assert(!prev_link);
  assert(!next_link);
  RoutineContext *prev = ThreadWorker::current_context;
  RoutineContext *next = this;
  ThreadWorker::current_context = next;
  prev_link = prev;
  prev_link->next_link = next;
#ifdef USE_UCONTEXT
  swapcontext(&prev->uctx, &next->uctx);
#else
  coctx_swap(&prev->ctx, &next->ctx);
#endif
}

void RoutineContext::switch_out() {
  assert(prev_link);
  RoutineContext *prev = this;
  RoutineContext *next = prev_link;
  ThreadWorker::current_context = next;
  prev_link->next_link = nullptr;
  prev_link = nullptr;
  assert(next_link == nullptr);
  next_link = nullptr;
#ifdef USE_UCONTEXT
  swapcontext(&prev->uctx, &next->uctx);
#else
  coctx_swap(&prev->ctx, &next->ctx);
#endif
}

} // namespace co