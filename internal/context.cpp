#include "context.h"
#include <assert.h>
#include <stdint.h>

namespace co {

extern "C" {
extern void coctx_swap(coctx_t *, coctx_t *) asm("coctx_swap");
};

typedef void (*ucontext_func_t) (void);

RoutineContext::RoutineContext() {
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
  func_ = func;
  arg_ = arg1;
  uintptr_t self = reinterpret_cast<uintptr_t>(this);
  uint32_t low = static_cast<uint32_t>(self);
  uint32_t high = static_cast<uint32_t>(self >> 32);
  makecontext(&uctx, (ucontext_func_t)&RoutineContext::Entry, 2, low, high);
#else
  coctx_make(&ctx, func, arg1, nullptr);
#endif
}

#ifdef USE_UCONTEXT
void RoutineContext::Entry(uint32_t low, uint32_t high) {
  uintptr_t self = (static_cast<uintptr_t>(high) << 32) | low;
  RoutineContext *ctx = reinterpret_cast<RoutineContext *>(self);
  assert(ctx);
  assert(ctx->func_);
  ctx->func_(ctx->arg_, nullptr);
}
#endif

void RoutineContext::Switch(RoutineContext& from, RoutineContext& to) {
#ifdef USE_UCONTEXT
  swapcontext(&from.uctx, &to.uctx);
#else
  coctx_swap(&from.ctx, &to.ctx);
#endif
}

} // namespace co
