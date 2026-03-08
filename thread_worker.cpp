#include "thread_worker.h"
#include "co_routine.h"
#include "task.h"
#include <queue>
#include <vector>

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

static void spawn_pending_tasks() {
    while (!pending_tasks.empty()) {
        std::unique_ptr<Task> task = std::move(pending_tasks.front());
        pending_tasks.pop_front();
        Task* raw = task.release();
        Coroutine* co = co_create([raw]() {
            std::unique_ptr<Task> t(raw);
            t->run();
            active_coroutine_count--;
            finished_coroutines.push_back(co_self());
        });
        active_coroutine_count++;
        co_resume(co);
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
