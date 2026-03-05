#pragma once
#include "coctx.h"

class RoutineContext {
public:
  explicit RoutineContext() : prev_link(nullptr), next_link(nullptr) {
    coctx_init(&ctx);
  }
  ~RoutineContext() {}
  coctx_t &GetContext() { return ctx; }
  void switch_in();
  void switch_out();

private:
  coctx_t ctx;
  RoutineContext *prev_link;
  RoutineContext *next_link;
};