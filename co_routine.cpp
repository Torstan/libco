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
#include "co_epoll.h"
#include "co_link.h"
#include "co_timeout.h"
#include "routine_context.h"
#include "thread_worker.h"
#include "util.h"

#include <map>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

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

static int CoRoutineFunc(void *arg, void *) {
  auto co = static_cast<Coroutine*>(arg);
  return co->Run();
}

int Coroutine::Run() {
  try {
    if (func_) {
      func_();
    }
  } catch (...) {
  }
  ended_ = true;
  co_yield_ct();
  return 0;
}

// Coroutine class implementation
Coroutine::Coroutine(std::function<void()>&& func)
    : func_(std::move(func)), started_(false), ended_(false), is_main_(false),
      enable_sys_hook_(false), sys_envs_(nullptr), stack_mem_(nullptr) {
  if (func_) {
    stack_mem_ = std::make_unique<StackMem>(kDefaultStackSize);
    routine_ctx_.InitCtx(stack_mem_->GetStackBuffer(), kDefaultStackSize);
  }
}

Coroutine::~Coroutine() {
}

void CoroutineDeleter::operator()(Coroutine *co) const { delete co; }

Coroutine *Coroutine::Create(std::function<void()>&& func) {
  try {
    if (!ThreadEnv::Current() && !ThreadEnv::Init()) {
      errno = ENOMEM;
      return nullptr;
    }
    return new Coroutine(std::move(func));
  } catch (const std::bad_alloc &) {
    errno = ENOMEM;
    return nullptr;
  }
}

Coroutine *Coroutine::Self() {
  if (!ThreadWorker::current_context)
    return nullptr;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
  return container_of(ThreadWorker::current_context, Coroutine, routine_ctx_);
#pragma GCC diagnostic pop
}

void Coroutine::Yield() { routine_ctx_.switch_out(); }

void Coroutine::Resume() {
  if (ended_) {
    return;
  }
  if (!started_) {
    routine_ctx_.MakeCtx((coctx_func_t)CoRoutineFunc, this);
    started_ = true;
  }
  routine_ctx_.switch_in();
}

void Coroutine::Reset() {
  if (is_main_ || !stack_mem_) {
    return;
  }
  started_ = false;
  ended_ = false;
  routine_ctx_.InitCtx(stack_mem_->GetStackBuffer(), kDefaultStackSize);
}

void Coroutine::Free() { delete this; }

int co_accept(int fd, struct sockaddr *addr, socklen_t *len) {
  return ::co_accept(fd, addr, len);
}

// ThreadEnv class implementation
ThreadEnv::ThreadEnv() : epoll_ctx_(std::make_unique<EpollCtx>()) {}

ThreadEnv::~ThreadEnv() {
  ThreadWorker::current_context = nullptr;
}

ThreadEnv *ThreadEnv::Current() { return gCoEnvPerThread.env; }

bool ThreadEnv::Init() {
  if (gCoEnvPerThread.env) {
    return true;
  }

  try {
    ThreadEnv *env = new ThreadEnv();
    try {
      env->main_coroutine_.reset(new Coroutine([](){}));
    } catch (...) {
      delete env;
      throw;
    }
    env->main_coroutine_->SetMain();
    ThreadWorker::current_context = &env->main_coroutine_->routine_ctx_;
    gCoEnvPerThread.env = env;
    return true;
  } catch (const std::bad_alloc &) {
    errno = ENOMEM;
    return false;
  }
}

// int poll(struct pollfd fds[], nfds_t nfds, int timeout);
//  { fd,events,revents }
struct PollItem;
struct PollBase : public TimeoutItem {
  struct pollfd *fds{nullptr};
  nfds_t nfds{0}; // typedef unsigned long int nfds_t;
  PollItem *poll_items{nullptr};
  int all_event_detach{0};
  int epoll_fd{0};
  int raise_cnt{0};
};
struct PollItem : public TimeoutItem {
  struct pollfd *self_pfd{nullptr};
  PollBase *poll{nullptr};
  int registered_fd{-1};
  bool owns_registered_fd{false};

  struct epoll_event ep_event;
};

typedef int (*poll_func_t)(struct pollfd fds[], nfds_t nfds, int timeout);

static int SystemPoll(struct pollfd fds[], nfds_t nfds, int timeout) {
  return ::poll(fds, nfds, timeout);
}

static int DupFdCloseOnExec(int fd) {
#ifdef F_DUPFD_CLOEXEC
  {
    int dup_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (dup_fd >= 0) {
      return dup_fd;
    }
  }
  int dup_errno = errno;
  if (dup_errno != EINVAL) {
    errno = dup_errno;
    return -1;
  }
#endif

  int dup_fd = dup(fd);
  if (dup_fd < 0) {
    return -1;
  }
  int flags = fcntl(dup_fd, F_GETFD);
  if (flags < 0) {
    int dup_errno = errno;
    close(dup_fd);
    errno = dup_errno;
    return -1;
  }
  if (fcntl(dup_fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
    int dup_errno = errno;
    close(dup_fd);
    errno = dup_errno;
    return -1;
  }
  return dup_fd;
}

static void PollProcessFunc(TimeoutItem *item);

static constexpr nfds_t kStackPollItemCount = 2;

class PollState {
 public:
  PollState(EpollCtx *ep_ctx, struct pollfd fds[], nfds_t nfds,
            Coroutine *owner)
      : poll_(std::make_unique<PollBase>()) {
    poll_->epoll_fd = ep_ctx->fd();
    poll_->fds = new pollfd[nfds];
    for (nfds_t i = 0; i < nfds; ++i) {
      poll_->fds[i] = fds[i];
      poll_->fds[i].revents = 0;
    }
    poll_->nfds = nfds;
    poll_->poll_items =
        nfds <= kStackPollItemCount ? stack_items_ : new PollItem[nfds];
    poll_->process_func = PollProcessFunc;
    poll_->arg = owner;
  }

  ~PollState() {
    if (poll_->poll_items != stack_items_) {
      delete[] poll_->poll_items;
      poll_->poll_items = nullptr;
    }
    delete[] poll_->fds;
    poll_->fds = nullptr;
  }

  PollBase *poll() { return poll_.get(); }

 private:
  std::unique_ptr<PollBase> poll_;
  PollItem stack_items_[kStackPollItemCount];
};
/*
 *   EPOLLPRI 		POLLPRI    // There is urgent data to read.
 *   EPOLLMSG 		POLLMSG
 *
 *   				POLLREMOVE
 *   				POLLRDHUP
 *   				POLLNVAL
 *
 * */
static uint32_t PollEvent2Epoll(short events) {
  uint32_t e = 0;
  if (events & POLLIN)
    e |= EPOLLIN;
  if (events & POLLOUT)
    e |= EPOLLOUT;
  if (events & POLLHUP)
    e |= EPOLLHUP;
  if (events & POLLERR)
    e |= EPOLLERR;
  if (events & POLLRDNORM)
    e |= EPOLLRDNORM;
  if (events & POLLWRNORM)
    e |= EPOLLWRNORM;
  return e;
}
static short EpollEvent2Poll(uint32_t events) {
  short e = 0;
  if (events & EPOLLIN)
    e |= POLLIN;
  if (events & EPOLLOUT)
    e |= POLLOUT;
  if (events & EPOLLHUP)
    e |= POLLHUP;
  if (events & EPOLLERR)
    e |= POLLERR;
  if (events & EPOLLRDNORM)
    e |= POLLRDNORM;
  if (events & EPOLLWRNORM)
    e |= POLLWRNORM;
  return e;
}

static void PollProcessFunc(TimeoutItem *item) {
  Coroutine *co = (Coroutine *)item->arg;
  co_resume(co);
}

static void PollPrepareFunc(TimeoutItem *timeout_item, struct epoll_event &e,
                            TimeoutItemLink *active) {
  PollItem *item = (PollItem *)timeout_item;
  item->self_pfd->revents = EpollEvent2Poll(e.events);
  PollBase *poll = item->poll;
  poll->raise_cnt++;

  if (!poll->all_event_detach) {
    poll->all_event_detach = 1;
    TimeoutItemLink::remove(poll);
    active->add_tail(poll);
  }
}

enum class PollRegisterResult {
  kRegistered,
  kFallback,
  kError,
};

static PollRegisterResult RegisterPollFds(EpollCtx *ep_ctx,
                                          struct pollfd fds[], nfds_t nfds,
                                          int timeout, poll_func_t poll_func,
                                          PollBase *poll,
                                          int *fallback_ret) {
  for (nfds_t i = 0; i < nfds; i++) {
    PollItem &item = poll->poll_items[i];
    item.self_pfd = poll->fds + i;
    item.poll = poll;
    item.registered_fd = -1;
    item.owns_registered_fd = false;

    item.prepare_func = PollPrepareFunc;
    struct epoll_event &ev = item.ep_event;

    if (fds[i].fd > -1) {
      ev.data.ptr = &item;
      ev.events = PollEvent2Epoll(fds[i].events);

      int ret = ep_ctx->add(fds[i].fd, &ev);
      int registered_fd = fds[i].fd;
      bool owns_registered_fd = false;
      if (ret < 0 && errno == EEXIST) {
        int dup_fd = DupFdCloseOnExec(fds[i].fd);
        if (dup_fd >= 0) {
          ret = ep_ctx->add(dup_fd, &ev);
          if (ret == 0) {
            registered_fd = dup_fd;
            owns_registered_fd = true;
          } else {
            int add_errno = errno;
            close(dup_fd);
            errno = add_errno;
          }
        }
        if (ret < 0) {
          return PollRegisterResult::kError;
        }
      }
      if (ret == 0) {
        item.registered_fd = registered_fd;
        item.owns_registered_fd = owns_registered_fd;
      }
      if (ret < 0 && nfds == 1) {
        int add_errno = errno;
        bool should_fallback = add_errno == EPERM;
        if (add_errno == EBADF) {
          should_fallback = fcntl(fds[i].fd, F_GETFD) == -1 && errno == EBADF;
        }
        errno = add_errno;
        if (should_fallback) {
          *fallback_ret = poll_func ? poll_func(fds, nfds, timeout)
                                    : SystemPoll(fds, nfds, 0);
          return PollRegisterResult::kFallback;
        }
      }
    }
    // if fail,the timeout would work
  }
  return PollRegisterResult::kRegistered;
}

static void CleanupPoll(EpollCtx *ep_ctx, struct pollfd fds[], PollBase *poll) {
  TimeoutItemLink::remove(poll);
  for (nfds_t i = 0; i < poll->nfds; i++) {
    PollItem &item = poll->poll_items[i];
    int fd = item.registered_fd;
    if (fd > -1) {
      ep_ctx->del(fd, &item.ep_event);
      if (item.owns_registered_fd) {
        close(fd);
      }
      item.registered_fd = -1;
      item.owns_registered_fd = false;
    }
    fds[i].revents = poll->fds[i].revents;
  }
}

int co_poll_inner(struct pollfd fds[], nfds_t nfds, int timeout,
                  poll_func_t poll_func) {
  if (timeout == 0) {
    return poll_func ? poll_func(fds, nfds, timeout)
                     : SystemPoll(fds, nfds, timeout);
  }
  EpollCtx *ep_ctx = co_get_epoll_ct();
  if (!ep_ctx) {
    errno = ENOMEM;
    return -1;
  }
  if (timeout < 0) {
    timeout = INT_MAX;
  }

  std::unique_ptr<PollState> state;
  try {
    state.reset(new PollState(ep_ctx, fds, nfds, co_self()));
  } catch (const std::bad_alloc &) {
    errno = ENOMEM;
    return -1;
  }
  PollBase *poll = state->poll();

  int fallback_ret = 0;
  PollRegisterResult register_result =
      RegisterPollFds(ep_ctx, fds, nfds, timeout, poll_func, poll,
                      &fallback_ret);
  if (register_result == PollRegisterResult::kFallback) {
    return fallback_ret;
  }
  if (register_result == PollRegisterResult::kError) {
    int register_errno = errno;
    CleanupPoll(ep_ctx, fds, poll);
    errno = register_errno;
    return -1;
  }

  unsigned long long now = GetTickMS();
  poll->expire_time_ms = now + timeout;
  int ret = ep_ctx->timeout()->AddItem(poll, now);
  int raise_cnt = 0;
  if (ret != 0) {
    co_log_err(
        "CO_ERR: AddItem ret %d now %lld timeout %d arg.expire_time_ms %lld",
        ret, now, timeout, poll->expire_time_ms);
    errno = EINVAL;
    raise_cnt = -1;

  } else {
    co_yield_ct();
    raise_cnt = poll->raise_cnt;
  }

  CleanupPoll(ep_ctx, fds, poll);
  return raise_cnt;
}

int co_poll(struct pollfd fds[], nfds_t nfds, int timeout_ms) {
  if (nfds <= 1) {
    return co_poll_inner(fds, nfds, timeout_ms, nullptr);
  }

  std::map<int, nfds_t> fd_to_merged_idx;
  std::unique_ptr<pollfd[]> fds_merge;
  try {
    fds_merge.reset(new pollfd[nfds]);
  } catch (const std::bad_alloc &) {
    errno = ENOMEM;
    return -1;
  }

  nfds_t nfds_merge = 0;
  bool has_duplicate = false;
  for (nfds_t i = 0; i < nfds; ++i) {
    fds[i].revents = 0;
    std::pair<std::map<int, nfds_t>::iterator, bool> ret;
    try {
      ret = fd_to_merged_idx.insert(std::make_pair(fds[i].fd, nfds_merge));
    } catch (const std::bad_alloc &) {
      errno = ENOMEM;
      return -1;
    }
    if (ret.second) {
      fds_merge[nfds_merge] = fds[i];
      fds_merge[nfds_merge].revents = 0;
      ++nfds_merge;
    } else {
      fds_merge[ret.first->second].events |= fds[i].events;
      has_duplicate = true;
    }
  }

  if (!has_duplicate) {
    return co_poll_inner(fds, nfds, timeout_ms, nullptr);
  }

  int ret = co_poll_inner(fds_merge.get(), nfds_merge, timeout_ms, nullptr);
  if (ret <= 0) {
    return ret;
  }

  ret = 0;
  const short always_reported = POLLERR | POLLHUP | POLLNVAL;
  for (nfds_t i = 0; i < nfds; ++i) {
    auto it = fd_to_merged_idx.find(fds[i].fd);
    if (it == fd_to_merged_idx.end()) {
      continue;
    }
    fds[i].revents =
        fds_merge[it->second].revents & (fds[i].events | always_reported);
    if (fds[i].revents) {
      ++ret;
    }
  }
  return ret;
}

static void CollectReadyEvents(EpollCtx *ep_ctx, int event_count,
                               TimeoutItemLink *active) {
  for (int i = 0; i < event_count; i++) {
    epoll_event &ev = ep_ctx->events()->events[i];
    TimeoutItem *item = (TimeoutItem *)ev.data.ptr;
    if (item->prepare_func) {
      item->prepare_func(item, ev, active);
    } else {
      active->add_tail(item);
    }
  }
}

static void CollectTimeouts(EpollCtx *ep_ctx, unsigned long long now,
                            TimeoutItemLink *timeout) {
  ep_ctx->timeout()->TakeAll(now, timeout);

  TimeoutItem *item = timeout->head;
  while (item) {
    item->timeout = true;
    item = item->next;
  }
}

static void DispatchActiveItems(EpollCtx *ep_ctx, unsigned long long now,
                                TimeoutItemLink *active) {
  TimeoutItem *item = active->head;
  while (item) {
    active->pop_head();
    if (item->timeout && now < item->expire_time_ms) {
      int ret = ep_ctx->timeout()->AddItem(item, now);
      if (!ret) {
        item->timeout = false;
        item = active->head;
        continue;
      }
    }
    if (item->process_func) {
      item->process_func(item);
    }

    item = active->head;
  }
}

void co_eventloop(pfn_co_eventloop_t func, void *arg) {
  EpollCtx *ep_ctx = co_get_epoll_ct();
  if (!ep_ctx) {
    errno = ENOMEM;
    return;
  }

  for (;;) {
    int ret = ep_ctx->wait(1);
    if (ret < 0) {
      int wait_errno = errno;
      if (wait_errno == EINTR) {
        ret = 0;
      } else {
        errno = wait_errno;
        break;
      }
    }

    TimeoutItemLink *active = ep_ctx->active_list();
    TimeoutItemLink *timeout = ep_ctx->timeout_list();
    timeout->clear();

    CollectReadyEvents(ep_ctx, ret, active);

    unsigned long long now = GetTickMS();
    CollectTimeouts(ep_ctx, now, timeout);

    active->join(*timeout);
    DispatchActiveItems(ep_ctx, now, active);

    if (func) {
      if (-1 == func(arg)) {
        break;
      }
    }
  }
}

EpollCtx *co_get_epoll_ct() {
  if (!ThreadEnv::Current() && !ThreadEnv::Init()) {
    return nullptr;
  }
  return ThreadEnv::Current()->Epoll();
}

} // namespace co
