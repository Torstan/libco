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

static thread_local ThreadEnv *gCoEnvPerThread = nullptr;

static int CoRoutineFunc(Coroutine *co, void *) { return co->Run(); }
int Coroutine::Run() {
  if (func_) {
    func_();
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
    constexpr int stack_size = 256 * 1024;
    stack_mem_ = new StackMem(stack_size);
    routine_ctx_.InitCtx(stack_mem_->GetStackBuffer(), stack_size);
  }
}

Coroutine::~Coroutine() {
  if (stack_mem_) {
    delete stack_mem_;
  }
}

Coroutine *Coroutine::Create(std::function<void()>&& func) {
  if (!ThreadEnv::Current()) {
    ThreadEnv::Init();
  }
  return new Coroutine(std::move(func));
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
  if (!started_) {
    routine_ctx_.MakeCtx((coctx_func_t)CoRoutineFunc, this);
    started_ = true;
  }
  routine_ctx_.switch_in();
}

void Coroutine::Free() { delete this; }

// ThreadEnv class implementation
ThreadEnv::ThreadEnv() : epoll_ctx_(new EpollCtx()) {}

ThreadEnv::~ThreadEnv() {
  if (epoll_ctx_) {
    delete epoll_ctx_;
  }
}

ThreadEnv *ThreadEnv::Current() { return gCoEnvPerThread; }

void ThreadEnv::Init() {
  ThreadEnv *env = new ThreadEnv();
  gCoEnvPerThread = env;

  Coroutine *self = new Coroutine([](){});
  self->SetMain();
  ThreadWorker::current_context = &self->routine_ctx_;
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

  struct epoll_event ep_event;
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

typedef int (*poll_func_t)(struct pollfd fds[], nfds_t nfds, int timeout);
int co_poll_inner(struct pollfd fds[], nfds_t nfds, int timeout,
                  poll_func_t poll_func) {
  EpollCtx *ep_ctx = co_get_epoll_ct();
  if (timeout == 0 && poll_func != nullptr) {
    return poll_func(fds, nfds, timeout);
  }
  if (timeout < 0) {
    timeout = INT_MAX;
  }
  int epfd = ep_ctx->fd();

  // 1.struct change
  PollBase *pb = new PollBase();
  PollBase &arg = *pb;

  arg.epoll_fd = epfd;
  arg.fds = new pollfd[nfds];
  arg.nfds = nfds;

  PollItem arr[2];
  if (nfds < sizeof(arr) / sizeof(arr[0])) {
    arg.poll_items = arr;
  } else {
    arg.poll_items = new PollItem[nfds];
  }

  arg.process_func = PollProcessFunc;
  arg.arg = co_self();

  // 2. add epoll
  for (nfds_t i = 0; i < nfds; i++) {
    arg.poll_items[i].self_pfd = arg.fds + i;
    arg.poll_items[i].poll = &arg;

    arg.poll_items[i].prepare_func = PollPrepareFunc;
    struct epoll_event &ev = arg.poll_items[i].ep_event;

    if (fds[i].fd > -1) {
      ev.data.ptr = arg.poll_items + i;
      ev.events = PollEvent2Epoll(fds[i].events);

      int ret = ep_ctx->add(fds[i].fd, &ev);
      if (ret < 0 && errno == EPERM && nfds == 1 && poll_func != nullptr) {
        if (arg.poll_items != arr) {
          delete[] (arg.poll_items);
          arg.poll_items = nullptr;
        }
        delete[] (arg.fds);
        delete (&arg);
        return poll_func(fds, nfds, timeout);
      }
    }
    // if fail,the timeout would work
  }

  // 3.add timeout
  unsigned long long now = GetTickMS();
  arg.expire_time_ms = now + timeout;
  int ret = ep_ctx->timeout()->AddItem(&arg, now);
  int raise_cnt = 0;
  if (ret != 0) {
    co_log_err(
        "CO_ERR: AddItem ret %d now %lld timeout %d arg.expire_time_ms %lld",
        ret, now, timeout, arg.expire_time_ms);
    errno = EINVAL;
    raise_cnt = -1;

  } else {
    co_yield_ct();
    raise_cnt = arg.raise_cnt;
  }

  {
    // clear epoll status and memory
    TimeoutItemLink::remove(&arg);
    for (nfds_t i = 0; i < nfds; i++) {
      int fd = fds[i].fd;
      if (fd > -1) {
        ep_ctx->del(fd, &arg.poll_items[i].ep_event);
      }
      fds[i].revents = arg.fds[i].revents;
    }

    if (arg.poll_items != arr) {
      delete[] (arg.poll_items);
      arg.poll_items = nullptr;
    }

    delete[] (arg.fds);
    delete (&arg);
  }
  return raise_cnt;
}

int co_poll(struct pollfd fds[], nfds_t nfds, int timeout_ms) {
  return co_poll_inner(fds, nfds, timeout_ms, nullptr);
}

void co_eventloop(pfn_co_eventloop_t func, void *arg) {
  EpollCtx *ep_ctx = co_get_epoll_ct();

  for (;;) {
    int ret = ep_ctx->wait(1);
    TimeoutItemLink *active = ep_ctx->active_list();
    TimeoutItemLink *timeout = ep_ctx->timeout_list();
    timeout->clear();

    for (int i = 0; i < ret; i++) {
      epoll_event &ev = ep_ctx->events()->events[i];
      TimeoutItem *item = (TimeoutItem *)ev.data.ptr;
      if (item->prepare_func) {
        item->prepare_func(item, ev, active);
      } else {
        active->add_tail(item);
      }
    }

    unsigned long long now = GetTickMS();
    ep_ctx->timeout()->TakeAll(now, timeout);

    TimeoutItem *item = timeout->head;
    while (item) {
      // printf("raise timeout %p\n", item);
      item->timeout = true;
      item = item->next;
    }

    active->join(*timeout);

    item = active->head;
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
    if (func) {
      if (-1 == func(arg)) {
        break;
      }
    }
  }
}
EpollCtx *co_get_epoll_ct() {
  if (!ThreadEnv::Current()) {
    ThreadEnv::Init();
  }
  return ThreadEnv::Current()->Epoll();
}
