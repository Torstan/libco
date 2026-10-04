#include "co_routine.h"
#include "co_future.h"
#include "co_cond.h"
#include "thread_worker.h"
#include "task.h"
#include "internal/io_backend.h"
#include "internal/event.h"
#include "internal/util.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>

// Observe actual Coroutine deletion, not destruction of the task capture.
static void *watched = nullptr;
static int deletions = 0;
static bool reject_allocations = false;
void *operator new(std::size_t size) {
  if (reject_allocations) throw std::bad_alloc();
  if (void *p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void *p) noexcept {
  if (p && p == watched) {
    if (p == co::co_self()) _exit(77);
    ++deletions;
  }
  std::free(p);
}
#if defined(__cpp_sized_deallocation)
void operator delete(void *p, std::size_t) noexcept { ::operator delete(p); }
#endif
static void require(bool ok) { if (!ok) std::exit(1); }
static void ready_main() {
  require(co::ThreadEnv::Init());
  auto f = co::make_ready_future<int>(42);
  f.wait();
  require(f.get() == 42);
}
static void pending_main() {
  require(co::ThreadEnv::Init());
  co::Promise<int> p;
  auto f = p.get_future();
  bool caught = false;
  try { f.wait(); } catch (const std::logic_error&) { caught = true; }
  require(caught);
  caught = false;
  try { (void)f.get(); } catch (const std::logic_error&) { caught = true; }
  require(caught);
  p.set_value(7);
  require(f.get() == 7);
}
static void ready_coroutine() {
  bool returned = false;
  auto *c = co::co_create([&] {
    auto f = co::make_ready_future<int>(42);
    f.wait();
    returned = f.get() == 42;
  });
  require(c != nullptr);
  co::co_resume(c);
  require(returned);
  co::co_free(c);
}
static void require_invalid_future(co::Future<int>& f) {
  bool done = false;
  auto *c = co::co_create([&] {
    int rejected = 0;
    try { (void)f.get(); } catch (const std::logic_error&) { ++rejected; }
    try { f.wait(); } catch (const std::logic_error&) { ++rejected; }
    try { (void)f.get_exception(); } catch (const std::logic_error&) { ++rejected; }
    require(rejected == 3);
    done = true;
  });
  co::co_resume(c);
  require(done); // Invalid Futures must reject the call without suspending.
  co::co_free(c);
}
static void consumed_ready_future() {
  auto f = co::make_ready_future<int>(42);
  require(f.get() == 42);
  require_invalid_future(f);
}
static void consumed_promise_future() {
  co::Promise<int> p;
  auto f = p.get_future();
  p.set_value(42);
  require(f.get() == 42);
  require_invalid_future(f);
}
static void moved_future() {
  co::Promise<int> p;
  auto f = p.get_future();
  auto moved = std::move(f);
  require_invalid_future(f);
  p.set_value(42);
  require(moved.get() == 42);
}
static void backend_instances() {
  int fds[2]; require(pipe(fds) == 0);
  co::EpollCtx a;
  co::IoEvent ev{}; ev.events = POLLIN;
  int first = 11, second = 22;
  ev.data = &first;
  require(a.add(fds[0], &ev) == 0);
  {
    co::EpollCtx b;
    ev.data = &second;
    require(b.add(fds[0], &ev) == 0);
    require(write(fds[1], "x", 1) == 1);
    require(b.wait(100) == 1);
    require(b.event(0).data == &second);
  }
  require(a.wait(100) == 1);
  require(a.event(0).data == &first);
  require(a.del(fds[0]) == 0);
  close(fds[0]); close(fds[1]);
}
static void task_reaping() {
  co::schedule(co::make_task([] { watched = co::co_self(); }));
  co::ThreadWorker w(0);
  w.run_loop(false);
  require(deletions == 1);
}
static void suspended_lifetime() {
  int runs = 0;
  auto *c = co::co_create([&] { ++runs; co::co_yield_ct(); ++runs; });
  co::co_resume(c);
  bool reset_rejected = false, free_rejected = false;
  try { c->Reset(); } catch (const std::logic_error&) { reset_rejected = true; }
  require(reset_rejected);
  try { co::co_free(c); } catch (const std::logic_error&) { free_rejected = true; }
  require(free_rejected);
  co::co_resume(c);
  require(runs == 2);
  c->Reset();
  co::co_resume(c);
  co::co_resume(c);
  require(runs == 4);
  co::co_free(c);
}
static void running_lifetime() {
  bool rejected = false;
  auto *c = co::co_create([&] {
    try { co::co_self()->Reset(); } catch (const std::logic_error&) { rejected = true; }
  });
  co::co_resume(c);
  require(rejected);
  co::co_free(c);
}
static int stop_when_done(void *arg) { return *static_cast<bool*>(arg) ? -1 : 0; }
static void pending_future_eventloop() {
  co::Promise<int> p;
  auto f = p.get_future();
  bool done = false;
  auto *c = co::co_create([&] { require(f.get() == 42); done = true; });
  co::co_resume(c);
  require(!done);
  co::Promise<int> moved(std::move(p));
  moved.set_value(42);
  co::co_eventloop(stop_when_done, &done);
  require(done);
  co::co_free(c);
}
static void abandoned_waiting_future() {
  auto p = std::make_unique<co::Promise<int>>();
  auto f = p->get_future();
  bool caught = false;
  auto *c = co::co_create([&] {
    try { (void)f.get(); } catch (const std::runtime_error&) { caught = true; }
  });
  co::co_resume(c);
  p.reset();
  co::co_eventloop(stop_when_done, &caught);
  require(caught);
  co::co_free(c);
}
static void condition_completion() {
  co::CoCond cond;
  int resumed = 0;
  bool done = false;
  auto body = [&] { require(cond.Timedwait(10) == 0); done = ++resumed == 2; };
  auto *a = co::co_create(body), *b = co::co_create(body);
  co::co_resume(a); co::co_resume(b);
  cond.Signal(); cond.Broadcast(); cond.Signal();
  co::co_eventloop(stop_when_done, &done);
  require(resumed == 2);
  co::co_free(a); co::co_free(b);
  done = false;
  auto *timer = co::co_create([&] { require(co::co_poll(nullptr, 0, 20) == 0); done = true; });
  co::co_resume(timer);
  co::co_eventloop(stop_when_done, &done);
  require(resumed == 2);
  co::co_free(timer);
}
static void future_single_waiter() {
  co::Promise<int> p;
  auto f = p.get_future();
  bool done = false;
  auto *c = co::co_create([&] { require(f.get() == 9); done = true; });
  co::co_resume(c);
  p.set_value(9);
  bool rejected = false;
  try { (void)f.get(); } catch (const std::logic_error&) { rejected = true; }
  require(rejected);
  co::co_eventloop(stop_when_done, &done);
  co::co_free(c);
}
static void reentrant_task_destruction() {
  struct ReentrantTask : co::Task {
    bool& destroyed;
    explicit ReentrantTask(bool& destroyed) : destroyed(destroyed) {}
    void run() override { watched = co::co_self(); }
    ~ReentrantTask() override {
      co::ThreadWorker nested(0);
      nested.run_loop(false);
      destroyed = true;
    }
  };
  bool destroyed = false;
  co::schedule(std::make_unique<ReentrantTask>(destroyed));
  co::ThreadWorker worker(0);
  worker.run_loop(false);
  require(destroyed && deletions == 1);
}
static void allocation_free_completion() {
  co::schedule(co::make_task([] {
    watched = co::co_self();
    reject_allocations = true;
  }));
  co::ThreadWorker worker(0);
  worker.run_loop(false);
  reject_allocations = false;
  require(deletions == 1);
}
static void suspended_task_destruction() {
  struct SuspendingTask : co::Task {
    bool& destroyed;
    explicit SuspendingTask(bool& destroyed) : destroyed(destroyed) {}
    void run() override { watched = co::co_self(); }
    ~SuspendingTask() override {
      require(co::co_poll(nullptr, 0, 5) == 0);
      destroyed = true;
    }
  };
  bool destroyed = false;
  co::schedule(std::make_unique<SuspendingTask>(destroyed));
  co::ThreadWorker worker(0);
  worker.run_loop(false);
  require(!destroyed && deletions == 0);
  co::co_eventloop(stop_when_done, &destroyed);
  require(destroyed && deletions == 1);
}
static void expired_wait_deadline() {
  bool done = false;
  auto *c = co::co_create([&] {
    co::detail::WaitRecord wait;
    co::detail::WaitTimer timer(wait);
    require(timer.Arm(co::GetTickMS() - 1) == 0);
    wait.Suspend();
    done = true;
  });
  co::co_resume(c);
  require(done);
  co::co_free(c);
}
static void timer_expired_only() {
  co::Timeout timers;
  co::TimerList expired;
  const auto now = co::GetTickMS();
  timers.TakeAll(now, &expired);
  co::TimerItem due, later;
  due.expire_time_ms = now + 59999;
  later.expire_time_ms = now + 90000;
  require(timers.AddItem(&due, now) == 0);
  require(timers.AddItem(&later, now) == 0);
  reject_allocations = true;
  timers.TakeAll(now + 59999, &expired);
  require(expired.pop_head() == &due);
  require(expired.empty());
  timers.TakeAll(now + 89999, &expired);
  require(expired.empty());
  timers.TakeAll(now + 90000, &expired);
  require(expired.pop_head() == &later);
  require(expired.empty());
  timers.TakeAll(now + 90000, &expired);
  require(expired.empty());
  reject_allocations = false;
}
static void timer_multiple_rotations() {
  co::Timeout timers;
  co::TimerList expired;
  const auto now = co::GetTickMS();
  timers.TakeAll(now, &expired);
  co::TimerItem item;
  item.expire_time_ms = now + 180000;
  require(timers.AddItem(&item, now) == 0);
  for (auto elapsed : {59999, 119998, 179997, 179999}) {
    timers.TakeAll(now + elapsed, &expired);
    require(expired.empty());
  }
  timers.TakeAll(now + 180000, &expired);
  require(expired.pop_head() == &item);
  require(expired.empty());
  timers.TakeAll(now + 240000, &expired);
  require(expired.empty());
}
static void timer_cancel_and_rearm() {
  co::Timeout timers;
  co::TimerList expired;
  const auto now = co::GetTickMS();
  timers.TakeAll(now, &expired);
  co::TimerItem item;
  item.expire_time_ms = now + 90000;
  require(timers.AddItem(&item, now) == 0);
  timers.TakeAll(now + 60000, &expired);
  require(expired.empty());
  co::Timeout::Remove(&item);
  co::Timeout::Remove(&item);
  timers.TakeAll(now + 90000, &expired);
  require(expired.empty());
  item.expire_time_ms = now + 90001;
  require(timers.AddItem(&item, now + 90000) == 0);
  timers.TakeAll(now + 90001, &expired);
  require(expired.pop_head() == &item);
  require(expired.empty());
}
int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  rlimit lim{0,0}; setrlimit(RLIMIT_CORE, &lim);
  struct Case { const char *group; const char *name; void (*run)(); } cases[] = {
    {"timer", "timer returns only expired items without allocating", timer_expired_only},
    {"timer", "timer survives multiple rotations and expires once", timer_multiple_rotations},
    {"timer", "reinserted timer can be canceled twice and rearmed", timer_cancel_and_rearm},
    {"deadline", "elapsed wait deadline completes without an error", expired_wait_deadline},
    {"oom", "task completion does not allocate", allocation_free_completion},
    {"reentrant", "suspending task destructor keeps its stack alive", suspended_task_destruction},
    {"reentrant", "task destructor may drive scheduling without freeing active stack", reentrant_task_destruction},
    {"exclusive", "ready result reserved for existing waiter", future_single_waiter},
    {"condition", "signal/broadcast cancels timers and resumes once", condition_completion},
    { "wait", "Future completed through event loop after Promise move", pending_future_eventloop},
    {"wait", "abandoned Promise wakes existing waiter", abandoned_waiting_future},
    {"future", "ready Future on main", ready_main},
    {"future", "pending Future on main", pending_main},
    {"future", "ready Future does not yield", ready_coroutine},
    {"future", "consumed ready Future rejects access without yielding", consumed_ready_future},
    {"future", "consumed Promise Future rejects access without yielding", consumed_promise_future},
    {"future", "moved-from Future rejects access without yielding", moved_future},
    {"backend", "backend registration isolation and destruction", backend_instances},
    { "lifecycle", "suspended Reset/Free rejected without damage", suspended_lifetime},
    {"lifecycle", "running Reset rejected", running_lifetime},
    {"reaping", "task coroutine reclaimed before run_loop returns", task_reaping}
  };
  int failures = 0;
  for (const auto &c : cases) {
    if (argc > 1 && std::strcmp(argv[1], c.group)) continue;
    pid_t pid = fork(); require(pid >= 0);
    if (!pid) { alarm(5); c.run(); _exit(0); }
    int s = 0; require(waitpid(pid, &s, 0) == pid);
    bool ok = WIFEXITED(s) && WEXITSTATUS(s) == 0;
    std::printf("%s: %s (status=%d)\n", ok ? "PASS" : "FAIL", c.name, s);
    failures += !ok;
  }
  return failures ? 1 : 0;
}
