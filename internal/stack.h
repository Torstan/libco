#pragma once
#include <new>
#include <stdlib.h>

namespace co {

class Coroutine;

class StackMem {
private:
  char *stack_buffer;

public:
  explicit StackMem(unsigned int stack_size_) {
    stack_buffer = (char *)malloc(stack_size_);
    if (!stack_buffer) {
      throw std::bad_alloc();
    }
  }
  ~StackMem() {
    free(stack_buffer);
    stack_buffer = nullptr;
  }
  char *GetStackBuffer() const { return stack_buffer; }
};

} // namespace co
