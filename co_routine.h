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

#include "co_stack.h"
#include "routine_context.h"
#include "util.h"
#include <stdint.h>
#include <sys/poll.h>
#include <functional>

typedef int (*pfn_co_eventloop_t)(void *);

class EpollCtx;

// Coroutine class - encapsulates coroutine state and lifecycle
class Coroutine {
public:
  // Create a new coroutine (initializes thread env if needed)
  static Coroutine *Create(std::function<void()>&& func);
  // Get the currently running coroutine on this thread
  static Coroutine *Self();

  int Run();

  // Yield from the current coroutine back to the scheduler
  void Yield();

  // Lifecycle
  void Resume();
  void Reset(); // allow re-use after timeout
  void Free();

  void EnableHook() { enable_sys_hook_ = true; }
  void DisableHook() { enable_sys_hook_ = false; }
  bool IsHookEnabled() const { return enable_sys_hook_; }

  void *&GetSysEnvs() { return sys_envs_; }
  // Internal: context access used by runtime internals
  void SetMain() { is_main_ = true; }

private:
  Coroutine(std::function<void()>&& func);
  ~Coroutine();

  RoutineContext routine_ctx_;
  std::function<void()> func_;

  bool started_;
  bool ended_;
  bool is_main_;
  bool enable_sys_hook_;
  void *sys_envs_;
  StackMem *stack_mem_;

  friend class ThreadEnv;
};

// ThreadEnv - per-thread coroutine environment (epoll + scheduler state)
class ThreadEnv {
public:
  static ThreadEnv *Current();
  static void Init();
  EpollCtx *Epoll() { return epoll_ctx_; }

private:
  ThreadEnv();
  ~ThreadEnv();
  EpollCtx *epoll_ctx_;
  friend class Coroutine;
};

// hook syscall ( poll/read/write/recv/send/recvfrom/sendto )
void co_enable_hook_sys();
void co_set_env_list(const char *name[], size_t cnt);

int co_poll(struct pollfd fds[], nfds_t nfds, int timeout_ms);
void co_eventloop(pfn_co_eventloop_t func, void *arg);

inline Coroutine* co_create(std::function<void()>&& func) {
  return Coroutine::Create(std::move(func));
}

inline void co_resume(Coroutine *co) { co->Resume(); }
inline void co_yield_ct() { Coroutine::Self()->Yield(); }
inline void co_free(Coroutine *co) { co->Free(); }
inline Coroutine *co_self() { return Coroutine::Self(); }
inline ThreadEnv *co_get_curr_thread_env() { return ThreadEnv::Current(); }

EpollCtx *co_get_epoll_ct(); // defined in co_routine.cpp
inline void co_disable_hook_sys() {
  if (auto *c = Coroutine::Self())
    c->DisableHook();
}
inline bool co_is_enable_sys_hook() {
  auto *c = Coroutine::Self();
  return c && c->IsHookEnabled();
}
