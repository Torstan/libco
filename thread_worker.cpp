#include "thread_worker.h"
#include "co_routine.h"
#include <system_error>
#include <cerrno>

namespace co {
ThreadWorker::ThreadWorker(int) {}
void ThreadWorker::run_loop(bool forever) {
  if (!ThreadEnv::Init()) {
    throw std::system_error(errno, std::generic_category(), "ThreadEnv::Init");
  }
  ThreadEnv::Current()->RunLoop(forever);
}
} // namespace co
