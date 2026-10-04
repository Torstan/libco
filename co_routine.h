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

#pragma once

#include <sys/poll.h>
#include <sys/socket.h>
#include <functional>
#include <memory>

namespace co {

typedef int (*pfn_co_eventloop_t)(void *);

class EpollCtx;
class Coroutine;
namespace detail {
class WaitTimer;
class WaitRecord;
Coroutine& RequireWaiter();
void*& CoroutineEnvs(Coroutine&);
}
struct CoroutineDeleter;
class ThreadEnvTls;
class ThreadEnv;
class ThreadWorker;
class Task;

// Coroutine class - encapsulates coroutine state and lifecycle
class Coroutine {
public:
  // Create a new coroutine (initializes thread env if needed)
  static Coroutine *Create(std::function<void()>&& func);
  // Get the currently running coroutine on this thread
  static Coroutine *Self();

  // Yield from the current coroutine back to its caller.
  void Yield();

  // Lifecycle
  void Resume();
  // Only caller-owned, unstarted or ended coroutines may be reset/freed.
  // Running, suspended, foreign-thread and scheduler-owned ones throw logic_error.
  void Reset();
  void Free();

  void EnableHook();
  void DisableHook();
  bool IsHookEnabled() const;

private:
  Coroutine(std::function<void()>&& func);
  ~Coroutine();

  struct Impl;
  std::unique_ptr<Impl> impl_;
  static int Entry(void*, void*);
  int Run();

  friend class ThreadEnv;
  friend Coroutine& detail::RequireWaiter();
  friend void*& detail::CoroutineEnvs(Coroutine&);
  friend class detail::WaitRecord;
  friend struct CoroutineDeleter;
};

// ThreadEnv - per-thread coroutine environment (epoll + scheduler state)
class ThreadEnv {
public:
  static ThreadEnv *Current();
  static bool Init();

private:
  ThreadEnv();
  ~ThreadEnv();
  struct Impl;
  std::unique_ptr<Impl> impl_;
  void Resume(Coroutine& co);
  void Yield(Coroutine& co);
  void RunTasks();
  void RunReady();
  void EventLoop(int (*callback)(void*), void *arg);
  void Reap();
  void RunLoop(bool forever);
  static int LoopCallback(void *arg);
  friend class Coroutine;
  friend class detail::WaitRecord;
  friend class detail::WaitTimer;
  friend void co_eventloop(int (*callback)(void*), void *arg);
  friend class ThreadWorker;
  friend class ThreadEnvTls;
  friend EpollCtx* co_get_epoll_ct();
  friend void schedule(std::unique_ptr<Task> task);
  friend void schedule_urgent(std::unique_ptr<Task> task);
};

// hook syscall ( poll/read/write/recv/send/recvfrom/sendto )
void co_enable_hook_sys();
void co_set_env_list(const char *name[], size_t cnt);

int co_poll(struct pollfd fds[], nfds_t nfds, int timeout_ms);
int co_accept(int fd, struct sockaddr *addr, socklen_t *len);
void co_eventloop(pfn_co_eventloop_t func, void *arg);

inline Coroutine* co_create(std::function<void()>&& func) {
  return Coroutine::Create(std::move(func));
}

inline void co_resume(Coroutine *co) { co->Resume(); }
inline void co_yield_ct() { Coroutine::Self()->Yield(); }
inline void co_free(Coroutine *co) { co->Free(); }
inline Coroutine *co_self() { return Coroutine::Self(); }
inline ThreadEnv *co_get_curr_thread_env() { return ThreadEnv::Current(); }

inline void co_disable_hook_sys() {
  if (auto *c = Coroutine::Self())
    c->DisableHook();
}
inline bool co_is_enable_sys_hook() {
  auto *c = Coroutine::Self();
  return c && c->IsHookEnabled();
}

} // namespace co
