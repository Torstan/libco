/*
* Tencent is pleased to support the open source community by making Libco
available.

* Copyright (C) 2014 THL A29 Limited, a Tencent company. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*	http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing,
* software distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "co_routine.h"
#include "internal/io_backend.h"
#include "internal/co_link.h"
#include "internal/timer_queue.h"
#include "internal/event.h"
#include "internal/context.h"
#include "internal/stack.h"
#include "internal/hook_state.h"
#include <deque>
#include "task.h"
#include "internal/poll.h"
#include "internal/util.h"

#include <memory>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <system_error>
#include <stdexcept>

#include <errno.h>
#include <poll.h>
#include <sys/time.h>

#include <assert.h>

#include <arpa/inet.h>

#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

int co_accept(int fd, struct sockaddr *addr, socklen_t *len);

namespace co {

class ThreadEnvTls {
public:
  ~ThreadEnvTls() {
    ThreadEnv *to_delete = env;
    env = nullptr;
    delete to_delete;
  }

  ThreadEnv *env{nullptr};
};

static thread_local ThreadEnvTls gCoEnvPerThread;
static constexpr int kDefaultStackSize = 256 * 1024;

struct Coroutine::Impl {
  RoutineContext routine_ctx_;
  ThreadEnv *owner_{ThreadEnv::Current()};
  Coroutine *caller_{nullptr};
  detail::WaitRecord *waiting_{nullptr};
  Coroutine *finished_next_{nullptr};
  bool auto_reap_{false};
  std::function<void()> func_;
  bool started_{false};
  bool ended_{false};
  bool is_main_{false};
  bool enable_sys_hook_{false};
  void *sys_envs_{nullptr};
  std::unique_ptr<StackMem> stack_mem_;
  explicit Impl(std::function<void()>&& func) : func_(std::move(func)) {
    if (func_) {
      stack_mem_ = std::make_unique<StackMem>(kDefaultStackSize);
      routine_ctx_.InitCtx(stack_mem_->GetStackBuffer(), kDefaultStackSize);
    }
  }
};

struct CoroutineDeleter {
  void operator()(Coroutine *co) const;
};

struct ThreadEnv::Impl {
  std::unique_ptr<EpollCtx> epoll_ctx_{std::make_unique<EpollCtx>()};
  std::unique_ptr<Coroutine, CoroutineDeleter> main_coroutine_;
  Coroutine *current_{nullptr};
  std::unique_ptr<Timeout> timers_{std::make_unique<Timeout>()};
  LinkedList<detail::WaitRecord> ready_;
  std::deque<std::unique_ptr<Task>> pending_tasks_;
  Coroutine *finished_coroutines_{nullptr};
};

void Coroutine::EnableHook() { impl_->enable_sys_hook_ = true; }
void Coroutine::DisableHook() { impl_->enable_sys_hook_ = false; }
bool Coroutine::IsHookEnabled() const { return impl_->enable_sys_hook_; }
void*& detail::CoroutineEnvs(Coroutine& co) { return co.impl_->sys_envs_; }

int Coroutine::Entry(void *arg, void *) {
  auto co = static_cast<Coroutine*>(arg);
  return co->Run();
}

int Coroutine::Run() {
  try {
    if (impl_->func_) {
      impl_->func_();
    }
  } catch (...) {
  }
  impl_->ended_ = true;
  co_yield_ct();
  return 0;
}

// Coroutine class implementation
Coroutine::Coroutine(std::function<void()>&& func)
    : impl_(std::make_unique<Impl>(std::move(func))) {}

Coroutine::~Coroutine() {
  co_cleanup_sys_envs(impl_->sys_envs_);
}

void CoroutineDeleter::operator()(Coroutine *co) const { delete co; }

Coroutine *Coroutine::Create(std::function<void()>&& func) {
  try {
    if (!ThreadEnv::Current() && !ThreadEnv::Init()) {
      if (errno == 0) {
        errno = ENOMEM;
      }
      return nullptr;
    }
    return new Coroutine(std::move(func));
  } catch (const std::bad_alloc &) {
    errno = ENOMEM;
    return nullptr;
  }
}

Coroutine *Coroutine::Self() {
  ThreadEnv* env = ThreadEnv::Current();
  return env ? env->impl_->current_ : nullptr;
}

void Coroutine::Yield() { impl_->owner_->Yield(*this); }
void Coroutine::Resume() { impl_->owner_->Resume(*this); }

void ThreadEnv::Resume(Coroutine& co) {
  if (Current() != this || co.impl_->is_main_ || co.impl_->owner_ != this || co.impl_->caller_ || co.impl_->waiting_ || impl_->current_ == &co) {
    throw std::logic_error("cannot resume an active or foreign coroutine");
  }
  if (co.impl_->ended_) return;
  if (!co.impl_->started_) {
    co.impl_->routine_ctx_.MakeCtx(Coroutine::Entry, &co);
    co.impl_->started_ = true;
  }
  Coroutine* previous = impl_->current_;
  co.impl_->caller_ = previous;
  impl_->current_ = &co;
  RoutineContext::Switch(previous->impl_->routine_ctx_, co.impl_->routine_ctx_);
  if (co.impl_->ended_ && co.impl_->auto_reap_) {
    // Task destruction has finished and its stack is no longer active.
    // Register for reaping without allocating or yielding.
    co.impl_->finished_next_ = impl_->finished_coroutines_;
    impl_->finished_coroutines_ = &co;
  }
}

void ThreadEnv::Yield(Coroutine& co) {
  if (Current() != this || impl_->current_ != &co || !co.impl_->caller_) {
    throw std::logic_error("cannot yield without a coroutine caller");
  }
  Coroutine* caller = co.impl_->caller_;
  co.impl_->caller_ = nullptr;
  impl_->current_ = caller;
  RoutineContext::Switch(co.impl_->routine_ctx_, caller->impl_->routine_ctx_);
}

void Coroutine::Reset() {
  if (ThreadEnv::Current() != impl_->owner_ || impl_->auto_reap_ || (impl_->started_ && !impl_->ended_)) {
    throw std::logic_error("cannot reset a scheduler-owned, running or suspended coroutine");
  }
  if (impl_->is_main_ || !impl_->stack_mem_) {
    return;
  }
  impl_->started_ = false;
  impl_->ended_ = false;
  impl_->routine_ctx_.InitCtx(impl_->stack_mem_->GetStackBuffer(), kDefaultStackSize);
}

void Coroutine::Free() {
  if (ThreadEnv::Current() != impl_->owner_ || impl_->is_main_ || impl_->auto_reap_ || (impl_->started_ && !impl_->ended_)) {
    throw std::logic_error("cannot free a main, scheduler-owned, running or suspended coroutine");
  }
  delete this;
}

int co_accept(int fd, struct sockaddr *addr, socklen_t *len) {
  return ::co_accept(fd, addr, len);
}

// ThreadEnv class implementation
ThreadEnv::ThreadEnv() : impl_(std::make_unique<Impl>()) {}

ThreadEnv::~ThreadEnv() {
  Reap();
}

ThreadEnv *ThreadEnv::Current() { return gCoEnvPerThread.env; }

bool ThreadEnv::Init() {
  if (gCoEnvPerThread.env) {
    return true;
  }

  try {
    ThreadEnv *env = new ThreadEnv();
    try {
      env->impl_->main_coroutine_.reset(new Coroutine({}));
    } catch (...) {
      delete env;
      throw;
    }
    env->impl_->main_coroutine_->impl_->is_main_ = true;
    env->impl_->main_coroutine_->impl_->owner_ = env;
    env->impl_->current_ = env->impl_->main_coroutine_.get();
    gCoEnvPerThread.env = env;
    return true;
  } catch (const std::bad_alloc &) {
    errno = ENOMEM;
    return false;
  } catch (const std::system_error &e) {
    errno = e.code().value();
    return false;
  }
}

void ThreadEnv::Reap() {
  while (auto* co = impl_->finished_coroutines_) {
    impl_->finished_coroutines_ = co->impl_->finished_next_;
    delete co;
  }
}

void ThreadEnv::RunTasks() {
  Reap();
  do {
    while (!impl_->pending_tasks_.empty()) {
      std::unique_ptr<Task> task = std::move(impl_->pending_tasks_.front());
      impl_->pending_tasks_.pop_front();
      Task* borrowed = task.get();
      Coroutine* co = co_create([borrowed] {
        std::unique_ptr<Task> owned(borrowed);
        owned->run();
      });
      if (!co) throw std::bad_alloc();
      co->impl_->auto_reap_ = true;
      task.release();
      co_resume(co);
    }
    RunReady();
    Reap();
  } while (!impl_->pending_tasks_.empty());
}

void ThreadEnv::RunReady() {
  while (auto* wait = impl_->ready_.pop_head()) {
    Coroutine* co = wait->coroutine_;
    wait->Detach();
    co->impl_->waiting_ = nullptr;
    Resume(*co);
  }
}

namespace detail {
WaitRecord::WaitRecord() : coroutine_(Coroutine::Self()), owner_(ThreadEnv::Current()) {
  if (!coroutine_ || coroutine_->impl_->is_main_) {
    throw std::logic_error("cannot wait on a not-ready Future without a coroutine context");
  }
  if (coroutine_->impl_->waiting_) throw std::logic_error("coroutine already waiting");
  coroutine_->impl_->waiting_ = this;
}
WaitRecord::~WaitRecord() {
  LinkedList<WaitRecord>::remove(this);
  Detach();
  if (coroutine_->impl_->waiting_ == this) coroutine_->impl_->waiting_ = nullptr;
}
void WaitRecord::SetCleanup(void (*cleanup)(void*) noexcept, void *arg) {
  cleanup_ = cleanup;
  arg_ = arg;
}
void WaitRecord::Detach() noexcept {
  auto cleanup = cleanup_;
  cleanup_ = nullptr;
  if (cleanup) cleanup(arg_);
}
void WaitRecord::Suspend() {
  if (owner_ != ThreadEnv::Current() || coroutine_ != Coroutine::Self()) {
    throw std::logic_error("wait belongs to another coroutine");
  }
  if (completed_) return;
  suspended_ = true;
  coroutine_->Yield();
}
void WaitRecord::Complete() {
  if (owner_ != ThreadEnv::Current()) {
    throw std::logic_error("cross-thread completion is unsupported");
  }
  if (completed_) return;
  completed_ = true;
  if (suspended_) owner_->impl_->ready_.add_tail(this);
}
int WaitTimer::Arm(unsigned long long deadline) {
  expire_time_ms = deadline;
  const auto now = GetTickMS();
  if (deadline <= now) {
    waiter_.Complete();
    return 0;
  }
  return waiter_.owner_->impl_->timers_->AddItem(this, now);
}
} // namespace detail

int ThreadEnv::LoopCallback(void* arg) {
  static_cast<ThreadEnv*>(arg)->RunTasks();
  return 0;
}

void ThreadEnv::RunLoop(bool forever) {
  RunTasks();
  if (forever) co_eventloop(LoopCallback, this);
}

void schedule(std::unique_ptr<Task> task) {
  if (!ThreadEnv::Init()) throw std::bad_alloc();
  ThreadEnv::Current()->impl_->pending_tasks_.push_back(std::move(task));
}
void schedule_urgent(std::unique_ptr<Task> task) {
  if (!ThreadEnv::Init()) throw std::bad_alloc();
  ThreadEnv::Current()->impl_->pending_tasks_.push_front(std::move(task));
}

int co_poll(struct pollfd fds[], nfds_t nfds, int timeout_ms) {
  return detail::PollWait(fds, nfds, timeout_ms, nullptr);
}

void ThreadEnv::EventLoop(pfn_co_eventloop_t func, void *arg) {
  for (;;) {
    int ret = impl_->epoll_ctx_->wait(1);
    if (ret < 0) {
      if (errno != EINTR) break;
      ret = 0;
    }
    // Collect the complete batch before any waiter may destroy registrations.
    for (int i = 0; i < ret; ++i) {
      auto ev = impl_->epoll_ctx_->event(i);
      auto* source = static_cast<detail::IoSource*>(ev.data);
      source->notify(source, ev.events);
    }
    auto now = GetTickMS();
    TimerList expired;
    impl_->timers_->TakeAll(now, &expired);
    while (auto* item = expired.pop_head()) {
      static_cast<detail::WaitTimer*>(item)->waiter_.Complete();
    }
    RunReady();
    Reap();
    if (func && func(arg) == -1) break;
  }
}

void co_eventloop(pfn_co_eventloop_t func, void *arg) {
  if (!ThreadEnv::Init()) return;
  ThreadEnv::Current()->EventLoop(func, arg);
}

EpollCtx *co_get_epoll_ct() {
  if (!ThreadEnv::Current() && !ThreadEnv::Init()) {
    return nullptr;
  }
  return ThreadEnv::Current()->impl_->epoll_ctx_.get();
}

} // namespace co
