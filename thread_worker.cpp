#include "thread_worker.h"

thread_local RoutineContext *ThreadWorker::current_context(nullptr);

ThreadWorker::ThreadWorker(int idx) : thread_idx(idx) {}
void ThreadWorker::RunLoop() {}