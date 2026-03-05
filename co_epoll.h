/*
* Tencent is pleased to support the open source community by making Libco
available.
*
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

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#if !defined(__APPLE__) && !defined(__FreeBSD__)
#include <sys/epoll.h>
#else
#include <sys/event.h>

// macOS/BSD: emulate epoll API with kqueue
enum EPOLL_EVENTS {
  EPOLLIN = 0X001,
  EPOLLPRI = 0X002,
  EPOLLOUT = 0X004,
  EPOLLERR = 0X008,
  EPOLLHUP = 0X010,
  EPOLLRDNORM = 0x40,
  EPOLLWRNORM = 0x004,
};

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

typedef union epoll_data {
  void *ptr;
  int fd;
  uint32_t u32;
  uint64_t u64;
} epoll_data_t;

struct epoll_event {
  uint32_t events;
  epoll_data_t data;
};
#endif

// Forward declarations
struct Timeout;
struct TimeoutItemLink;

// Event result buffer (internal)
struct co_epoll_res {
  int size;
  struct epoll_event *events;
  struct kevent *eventlist; // only used on macOS/BSD
};

// Epoll context for one thread's event loop
class EpollCtx {
public:
  static constexpr int MAX_EVENTS = 1024 * 10;

  EpollCtx();
  ~EpollCtx();

  // Wait for events (timeout in ms). Returns number of ready events.
  int wait(int timeout_ms = 1);

  // Register/unregister file descriptors
  int add(int fd, struct epoll_event *ev);
  int del(int fd, struct epoll_event *ev);
  int mod(int fd, struct epoll_event *ev);

  // Accessors
  co_epoll_res *events() { return result_; }
  TimeoutItemLink *active_list() { return active_list_; }
  TimeoutItemLink *timeout_list() { return timeout_list_; }
  Timeout *timeout() { return timeout_; }
  int fd() const { return epoll_fd_; }

private:
  int epoll_fd_;
  Timeout *timeout_;
  TimeoutItemLink *active_list_;
  TimeoutItemLink *timeout_list_;
  co_epoll_res *result_;
};
