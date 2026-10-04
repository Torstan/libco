#include "co_routine.h"

#include <sys/socket.h>
#include <stdexcept>

using namespace co;

int main() {
  int (*accept_fn)(int, struct sockaddr *, socklen_t *) = co_accept;
  (void)accept_fn;

  Coroutine *initializer = co_create([]() {});
  Coroutine *main_routine = co_self();
  if (main_routine) {
    main_routine->Reset();
  }

  bool ran = false;
  Coroutine *routine = co_create([&ran]() {
    ran = true;
    co_yield_ct();
  });
  co_resume(routine);
  if (!ran) {
    return 1;
  }
  bool rejected = false;
  try { routine->Reset(); } catch (const std::logic_error&) { rejected = true; }
  if (!rejected) return 1;
  co_resume(routine);
  routine->Reset();
  co_free(routine);
  co_free(initializer);

  return 0;
}
