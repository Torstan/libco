#include "routine_context.h"
#include "thread_worker.h"
#include <assert.h>

extern "C" {
extern void coctx_swap(coctx_t *, coctx_t *) asm("coctx_swap");
};

void RoutineContext::switch_in() {
  assert(!prev_link);
  assert(!next_link);
  RoutineContext *prev = ThreadWorker::current_context;
  RoutineContext *next = this;
  ThreadWorker::current_context = next;
  prev_link = prev;
  prev_link->next_link = next;
  coctx_swap(&prev->ctx, &next->ctx);
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
  coctx_swap(&prev->ctx, &next->ctx);
}