#include "thread_worker.h"
#include "co_routine.h"
#include "task.h"
#include <queue>
#include <vector>

namespace co {

thread_local RoutineContext *ThreadWorker::current_context(nullptr);
static thread_local std::deque<std::unique_ptr<Task>> pending_tasks;
static thread_local int active_coroutine_count = 0;
static thread_local std::vector<Coroutine*> finished_coroutines;

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

static void cleanup_finished_coroutines() {
    for (auto* co : finished_coroutines) {
        co_free(co);
    }
    finished_coroutines.clear();
}

static void mark_current_coroutine_finished() {
    active_coroutine_count--;
    finished_coroutines.push_back(co_self());
}

static void run_task_and_mark_finished(std::unique_ptr<Task> task) {
    try {
        task->run();
    } catch (...) {
        mark_current_coroutine_finished();
        throw;
    }
    mark_current_coroutine_finished();
}

static Coroutine* create_task_coroutine(std::unique_ptr<Task> task) {
    Task* task_ptr = task.release();
    return co_create([task_ptr]() {
        std::unique_ptr<Task> owned_task(task_ptr);
        run_task_and_mark_finished(std::move(owned_task));
    });
}

static void spawn_pending_tasks() {
    while (!pending_tasks.empty()) {
        std::unique_ptr<Task> task = std::move(pending_tasks.front());
        pending_tasks.pop_front();
        Coroutine* coroutine = create_task_coroutine(std::move(task));
        active_coroutine_count++;
        co_resume(coroutine);
    }
}

static int eventloop_callback(void*) {
    cleanup_finished_coroutines();
    spawn_pending_tasks();
    return 0;
}

void ThreadWorker::run_loop(bool forever) {
    eventloop_callback(nullptr);
    if (forever) {
        co_eventloop(eventloop_callback, nullptr);
    }
}
void schedule(std::unique_ptr<Task> t) {
    add_task(std::move(t));
}
void schedule_urgent(std::unique_ptr<Task> t) {
    add_urgent_task(std::move(t));
}

} // namespace co
