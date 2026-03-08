#include "thread_worker.h"
#include "task.h"
#include <queue>

thread_local RoutineContext *ThreadWorker::current_context(nullptr);
static thread_local std::deque<std::unique_ptr<Task>> pending_tasks;

void add_task(std::unique_ptr<Task>&& t) {
    pending_tasks.push_back(std::move(t));
}
void add_urgent_task(std::unique_ptr<Task>&& t) {
    pending_tasks.push_front(std::move(t));
}

ThreadWorker::ThreadWorker(int idx) : thread_idx(idx) {}

void ThreadWorker::switch_in(RoutineContext* ctx) {
    ctx->switch_in();
}
void ThreadWorker::switch_out(RoutineContext* ctx) {
    ctx->switch_out();
}

void ThreadWorker::run_loop() {
    while (!pending_tasks.empty()) {
        auto task = std::move(pending_tasks.front());
        pending_tasks.pop_front();

        task->run();
    }
}

void schedule(std::unique_ptr<Task> t) {
    add_task(std::move(t));
}
void schedule_urgent(std::unique_ptr<Task> t) {
    add_urgent_task(std::move(t));
}