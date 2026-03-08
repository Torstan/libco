/*
 * Minimal libco test - just create and resume a coroutine
 */

#include "co_routine.h"
#include <iostream>

void *test_routine() {
  std::cout << "Coroutine started!" << std::endl;
  co_yield_ct();
  std::cout << "Coroutine resumed!" << std::endl;
  return nullptr;
}

int main() {
  std::cout << "Creating coroutine..." << std::endl;

  Coroutine *co = co_create([]() {
    test_routine();
  });

  std::cout << "Resuming coroutine..." << std::endl;
  co_resume(co);

  std::cout << "Back to main!" << std::endl;

  co_free(co);

  std::cout << "Test completed!" << std::endl;
  return 0;
}
