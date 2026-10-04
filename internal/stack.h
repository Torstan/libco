#pragma once
#include <new>
#include <stdlib.h>

namespace co {

class Coroutine;

class StackMem {
private:
  int stack_size;
  char *stack_bp; // stack_buffer + stack_size
  char *stack_buffer;

public:
  explicit StackMem(unsigned int stack_size_) {
    stack_size = stack_size_;
    stack_buffer = (char *)malloc(stack_size);
    if (!stack_buffer) {
      throw std::bad_alloc();
    }
    stack_bp = stack_buffer + stack_size;
  }
  ~StackMem() {
    free(stack_buffer);
    stack_buffer = nullptr;
  }
  char *GetStackBuffer() const { return stack_buffer; }
};

} // namespace co
