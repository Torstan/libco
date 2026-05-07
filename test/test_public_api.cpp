#include "co_routine.h"

#include <sys/socket.h>

using namespace co;

int main() {
  int (*accept_fn)(int, struct sockaddr *, socklen_t *) = co_accept;
  (void)accept_fn;

  Coroutine *routine = co_create([]() {});
  routine->Reset();
  co_free(routine);

  return 0;
}
