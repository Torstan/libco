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

#include "io_backend.h"
#include <errno.h>
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system_error>
#include <unistd.h>
#include <cassert>
#include <cstdint>

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


namespace co {
struct co_epoll_res {
  int size;
  epoll_event *events;
  struct kevent *eventlist;
};


#if !defined(__APPLE__) && !defined(__FreeBSD__)

static int co_epoll_wait(int epfd, struct co_epoll_res *events, int maxevents,
                         int timeout) {
  return epoll_wait(epfd, events->events, maxevents, timeout);
}
static int co_epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev,
                        void *registrations) {
  return epoll_ctl(epfd, op, fd, ev);
}
static int co_epoll_create(int size) { return epoll_create(size); }

static struct co_epoll_res *co_epoll_res_alloc(int n) {
  struct co_epoll_res *ptr =
      (struct co_epoll_res *)malloc(sizeof(struct co_epoll_res));
  if (!ptr) {
    return nullptr;
  }

  ptr->size = n;
  ptr->events = (struct epoll_event *)calloc(1, n * sizeof(struct epoll_event));
  ptr->eventlist = nullptr;
  if (!ptr->events) {
    free(ptr);
    return nullptr;
  }

  return ptr;
}
static void co_epoll_res_free(struct co_epoll_res *ptr) {
  if (!ptr)
    return;
  if (ptr->events)
    free(ptr->events);
  free(ptr);
}

#else
class clsFdMap // million of fd , 1024 * 1024
{
private:
  static const int row_size = 1024;
  static const int col_size = 1024;

  void **m_pp[1024];

public:
  clsFdMap() { memset(m_pp, 0, sizeof(m_pp)); }
  ~clsFdMap() {
    for (int i = 0; i < sizeof(m_pp) / sizeof(m_pp[0]); i++) {
      if (m_pp[i]) {
        for (int j = 0; j < col_size; ++j) free(m_pp[i][j]);
        free(m_pp[i]);
        m_pp[i] = nullptr;
      }
    }
  }
  inline int clear(int fd) {
    set(fd, nullptr);
    return 0;
  }
  inline int set(int fd, const void *ptr) {
    int idx = fd / row_size;
    if (idx < 0 || idx >= sizeof(m_pp) / sizeof(m_pp[0])) {
      assert(__LINE__ == 0);
      return -__LINE__;
    }
    if (!m_pp[idx]) {
      m_pp[idx] = (void **)calloc(1, sizeof(void *) * col_size);
    }
    m_pp[idx][fd % col_size] = (void *)ptr;
    return 0;
  }
  inline void *get(int fd) {
    int idx = fd / row_size;
    if (idx < 0 || idx >= sizeof(m_pp) / sizeof(m_pp[0])) {
      return nullptr;
    }
    void **lp = m_pp[idx];
    if (!lp)
      return nullptr;

    return lp[fd % col_size];
  }
};

struct kevent_pair_t {
  int fire_idx;
  int events;
  uint64_t u64;
  bool active; // Failed cleanup retains storage, but must not dispatch user data.
};
static int co_epoll_create(int size) { return kqueue(); }
static struct timespec milliseconds_to_timespec(int timeout_ms) {
  struct timespec t = {0};
  if (timeout_ms > 0) {
    t.tv_sec = timeout_ms / 1000;
    t.tv_nsec = (timeout_ms % 1000) * 1000000;
  }
  return t;
}

static int co_epoll_wait(int epfd, struct co_epoll_res *events, int maxevents,
                         int timeout) {
  struct timespec t = milliseconds_to_timespec(timeout);
  int ret = kevent(epfd, nullptr, 0,              // register null
                   events->eventlist, maxevents, // just retrival
                   (-1 == timeout) ? nullptr : &t);
  if (ret <= 0) {
    return ret;
  }

  int j = 0;
  for (int i = 0; i < ret; i++) {
    struct kevent &kev = events->eventlist[i];
    struct kevent_pair_t *ptr = (struct kevent_pair_t *)kev.udata;
    if (!ptr) {
      errno = EINVAL;
      return -1;
    }
    if (!ptr->active) continue;

    struct epoll_event *ev = nullptr;
    if (0 == ptr->fire_idx) {
      ev = events->events + j;
      ptr->fire_idx = j + 1;
      memset(ev, 0, sizeof(*ev));
      ++j;
    } else {
      ev = events->events + ptr->fire_idx - 1;
    }
    if (EVFILT_READ == kev.filter) {
      ev->events |= EPOLLIN;
    } else if (EVFILT_WRITE == kev.filter) {
      ev->events |= EPOLLOUT;
    }
    ev->data.u64 = ptr->u64;
  }
  for (int i = 0; i < ret; i++) {
    ((struct kevent_pair_t *)(events->eventlist[i].udata))->fire_idx = 0;
  }
  return j;
}
static int set_filters(int epfd, int fd, kevent_pair_t *ptr, int events) {
  struct timespec t = {0};
  for (int bit : {EPOLLIN, EPOLLOUT}) {
    if (!((ptr->events | events) & bit)) continue;
    bool adding = (events & bit) != 0;
    struct kevent kev = {0};
    EV_SET(&kev, fd, bit == EPOLLIN ? EVFILT_READ : EVFILT_WRITE,
           adding ? EV_ADD : EV_DELETE, 0, 0, ptr);
    int ret;
    do {
      ret = kevent(epfd, &kev, 1, nullptr, 0, &t);
    } while (ret < 0 && errno == EINTR);
    // Closing an fd already removes its filters from kqueue.
    if (ret < 0 && (adding || (errno != ENOENT && errno != EBADF))) return -1;
    if (adding) ptr->events |= bit;
    else ptr->events &= ~bit;
  }
  return 0;
}
static int co_epoll_del(int epfd, int fd, clsFdMap *fd_map) {
  struct kevent_pair_t *ptr = (struct kevent_pair_t *)fd_map->get(fd);
  if (!ptr)
    return 0;
  ptr->active = false;
  if (set_filters(epfd, fd, ptr, 0) < 0) return -1;
  fd_map->clear(fd);
  free(ptr);
  return 0;
}
static int co_epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev,
                        clsFdMap *fd_map) {
  if (EPOLL_CTL_DEL == op) {
    return co_epoll_del(epfd, fd, fd_map);
  }

  const int flags = (EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP);
  if (ev->events & ~flags) {
    errno = EINVAL;
    return -1;
  }

  auto *ptr = (kevent_pair_t *)fd_map->get(fd);
  if (ptr && !ptr->active) {
    if (co_epoll_del(epfd, fd, fd_map) < 0) return -1;
    ptr = nullptr;
  }
  if (ptr) {
    errno = EEXIST;
    return -1;
  }

  ptr = (kevent_pair_t *)calloc(1, sizeof(kevent_pair_t));
  fd_map->set(fd, ptr);

  if (set_filters(epfd, fd, ptr, ev->events) < 0) {
    int error = errno;
    co_epoll_del(epfd, fd, fd_map);
    errno = error;
    return -1;
  }

  ptr->u64 = ev->data.u64;
  ptr->active = true;

  return 0;
}

static struct co_epoll_res *co_epoll_res_alloc(int n) {
  struct co_epoll_res *ptr =
      (struct co_epoll_res *)malloc(sizeof(struct co_epoll_res));
  if (!ptr) {
    return nullptr;
  }

  ptr->size = n;
  ptr->events = (struct epoll_event *)calloc(1, n * sizeof(struct epoll_event));
  ptr->eventlist = (struct kevent *)calloc(1, n * sizeof(struct kevent));
  if (!ptr->events || !ptr->eventlist) {
    if (ptr->events) {
      free(ptr->events);
    }
    if (ptr->eventlist) {
      free(ptr->eventlist);
    }
    free(ptr);
    return nullptr;
  }

  return ptr;
}

static void co_epoll_res_free(struct co_epoll_res *ptr) {
  if (!ptr)
    return;
  if (ptr->events)
    free(ptr->events);
  if (ptr->eventlist)
    free(ptr->eventlist);
  free(ptr);
}

#endif

#if defined(__APPLE__) || defined(__FreeBSD__)
struct BackendRegistrations : clsFdMap {};
#else
struct BackendRegistrations {};
#endif

static uint32_t NativeEvents(short events) {
  uint32_t native = 0;
  if (events & POLLIN) native |= EPOLLIN;
  if (events & POLLOUT) native |= EPOLLOUT;
  if (events & POLLERR) native |= EPOLLERR;
  if (events & POLLHUP) native |= EPOLLHUP;
  if (events & POLLRDNORM) native |= EPOLLRDNORM;
  if (events & POLLWRNORM) native |= EPOLLWRNORM;
  return native;
}
static short PollEvents(uint32_t events) {
  short result = 0;
  if (events & EPOLLIN) result |= POLLIN;
  if (events & EPOLLOUT) result |= POLLOUT;
  if (events & EPOLLERR) result |= POLLERR;
  if (events & EPOLLHUP) result |= POLLHUP;
  if (events & EPOLLRDNORM) result |= POLLRDNORM;
  if (events & EPOLLWRNORM) result |= POLLWRNORM;
  return result;
}

struct EpollCtx::Impl {
  BackendRegistrations registrations;
  int fd{-1};
  co_epoll_res *result{nullptr};
  ~Impl() {
    co_epoll_res_free(result);
    if (fd >= 0) close(fd);
  }
};
EpollCtx::EpollCtx() : impl_(std::make_unique<Impl>()) {
  impl_->fd = co_epoll_create(MAX_EVENTS);
  if (impl_->fd < 0) {
    int error = errno;
    if (error == ENOMEM) throw std::bad_alloc();
    throw std::system_error(error, std::generic_category(), "co_epoll_create");
  }
}
EpollCtx::~EpollCtx() = default;
int EpollCtx::wait(int timeout_ms) {
  if (!impl_->result) {
    impl_->result = co_epoll_res_alloc(MAX_EVENTS);
    if (!impl_->result) { errno = ENOMEM; return -1; }
  }
  return co_epoll_wait(impl_->fd, impl_->result, MAX_EVENTS, timeout_ms);
}
IoEvent EpollCtx::event(int index) const {
  const auto& native = impl_->result->events[index];
  return {PollEvents(native.events), native.data.ptr};
}
int EpollCtx::fd() const { return impl_->fd; }
int EpollCtx::add(int fd, const IoEvent *event) {
  epoll_event native{};
  native.events = NativeEvents(event->events);
  native.data.ptr = event->data;
  return co_epoll_ctl(impl_->fd, EPOLL_CTL_ADD, fd, &native, &impl_->registrations);
}
int EpollCtx::del(int fd) {
  return co_epoll_ctl(impl_->fd, EPOLL_CTL_DEL, fd, nullptr, &impl_->registrations);
}
} // namespace co
