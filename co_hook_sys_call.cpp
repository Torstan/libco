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

#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/un.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <errno.h>
#include <netinet/in.h>
#include <time.h>

#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "co_routine.h"
#include "util.h"
#include <map>
#include <time.h>

using namespace co;

struct rpchook_t {
  int user_flag;
  struct sockaddr_in dest; // maybe sockaddr_un;
  int domain;              // AF_LOCAL , AF_INET

  struct timeval read_timeout;
  struct timeval write_timeout;
};
static rpchook_t *g_rpchook_socket_fd[102400] = {0};

typedef int (*socket_pfn_t)(int domain, int type, int protocol);
typedef int (*connect_pfn_t)(int socket, const struct sockaddr *address,
                             socklen_t address_len);
typedef int (*close_pfn_t)(int fd);

typedef ssize_t (*read_pfn_t)(int fildes, void *buf, size_t nbyte);
typedef ssize_t (*write_pfn_t)(int fildes, const void *buf, size_t nbyte);

typedef ssize_t (*sendto_pfn_t)(int socket, const void *message, size_t length,
                                int flags, const struct sockaddr *dest_addr,
                                socklen_t dest_len);

typedef ssize_t (*recvfrom_pfn_t)(int socket, void *buffer, size_t length,
                                  int flags, struct sockaddr *address,
                                  socklen_t *address_len);

typedef ssize_t (*send_pfn_t)(int socket, const void *buffer, size_t length,
                              int flags);
typedef ssize_t (*recv_pfn_t)(int socket, void *buffer, size_t length,
                              int flags);

typedef int (*poll_pfn_t)(struct pollfd fds[], nfds_t nfds, int timeout);
typedef int (*setsockopt_pfn_t)(int socket, int level, int option_name,
                                const void *option_value, socklen_t option_len);

typedef int (*fcntl_pfn_t)(int fildes, int cmd, ...);

typedef int (*setenv_pfn_t)(const char *name, const char *value, int overwrite);
typedef int (*unsetenv_pfn_t)(const char *name);
typedef char *(*getenv_pfn_t)(const char *name);
typedef int (*__poll_pfn_t)(struct pollfd fds[], nfds_t nfds, int timeout);

static socket_pfn_t g_sys_socket_func =
    (socket_pfn_t)dlsym(RTLD_NEXT, "socket");
static connect_pfn_t g_sys_connect_func =
    (connect_pfn_t)dlsym(RTLD_NEXT, "connect");
static close_pfn_t g_sys_close_func = (close_pfn_t)dlsym(RTLD_NEXT, "close");

static read_pfn_t g_sys_read_func = (read_pfn_t)dlsym(RTLD_NEXT, "read");
static write_pfn_t g_sys_write_func = (write_pfn_t)dlsym(RTLD_NEXT, "write");

static sendto_pfn_t g_sys_sendto_func =
    (sendto_pfn_t)dlsym(RTLD_NEXT, "sendto");
static recvfrom_pfn_t g_sys_recvfrom_func =
    (recvfrom_pfn_t)dlsym(RTLD_NEXT, "recvfrom");

static send_pfn_t g_sys_send_func = (send_pfn_t)dlsym(RTLD_NEXT, "send");
static recv_pfn_t g_sys_recv_func = (recv_pfn_t)dlsym(RTLD_NEXT, "recv");

static poll_pfn_t g_sys_poll_func = (poll_pfn_t)dlsym(RTLD_NEXT, "poll");

static setsockopt_pfn_t g_sys_setsockopt_func =
    (setsockopt_pfn_t)dlsym(RTLD_NEXT, "setsockopt");
static fcntl_pfn_t g_sys_fcntl_func = (fcntl_pfn_t)dlsym(RTLD_NEXT, "fcntl");

static setenv_pfn_t g_sys_setenv_func =
    (setenv_pfn_t)dlsym(RTLD_NEXT, "setenv");
static unsetenv_pfn_t g_sys_unsetenv_func =
    (unsetenv_pfn_t)dlsym(RTLD_NEXT, "unsetenv");
static getenv_pfn_t g_sys_getenv_func =
    (getenv_pfn_t)dlsym(RTLD_NEXT, "getenv");
static __poll_pfn_t g_sys___poll_func =
    (__poll_pfn_t)dlsym(RTLD_NEXT, "__poll");

#define HOOK_SYS_FUNC(name)                                                    \
  if (!g_sys_##name##_func) {                                                  \
    g_sys_##name##_func = (name##_pfn_t)dlsym(RTLD_NEXT, #name);               \
  }

static inline rpchook_t *get_by_fd(int fd) {
  if (fd > -1 && fd < (int)sizeof(g_rpchook_socket_fd) /
                          (int)sizeof(g_rpchook_socket_fd[0])) {
    return g_rpchook_socket_fd[fd];
  }
  return nullptr;
}
static inline rpchook_t *alloc_by_fd(int fd) {
  if (fd > -1 && fd < (int)sizeof(g_rpchook_socket_fd) /
                          (int)sizeof(g_rpchook_socket_fd[0])) {
    rpchook_t *lp = (rpchook_t *)calloc(1, sizeof(rpchook_t));
    lp->read_timeout.tv_sec = 1;
    lp->write_timeout.tv_sec = 1;
    g_rpchook_socket_fd[fd] = lp;
    return lp;
  }
  return nullptr;
}
static inline void free_by_fd(int fd) {
  if (fd > -1 && fd < (int)sizeof(g_rpchook_socket_fd) /
                          (int)sizeof(g_rpchook_socket_fd[0])) {
    rpchook_t *lp = g_rpchook_socket_fd[fd];
    if (lp) {
      g_rpchook_socket_fd[fd] = nullptr;
      free(lp);
    }
  }
  return;
}
int socket(int domain, int type, int protocol) {
  HOOK_SYS_FUNC(socket);

  if (!co_is_enable_sys_hook()) {
    return g_sys_socket_func(domain, type, protocol);
  }
  int fd = g_sys_socket_func(domain, type, protocol);
  if (fd < 0) {
    return fd;
  }

  rpchook_t *lp = alloc_by_fd(fd);
  lp->domain = domain;

  fcntl(fd, F_SETFL, g_sys_fcntl_func(fd, F_GETFL, 0));

  return fd;
}

int co_accept(int fd, struct sockaddr *addr, socklen_t *len) {
  int cli = accept(fd, addr, len);
  if (cli < 0) {
    return cli;
  }
  alloc_by_fd(cli);
  return cli;
}
int connect(int fd, const struct sockaddr *address, socklen_t address_len) {
  HOOK_SYS_FUNC(connect);

  if (!co_is_enable_sys_hook()) {
    return g_sys_connect_func(fd, address, address_len);
  }

  // 1.sys call
  int ret = g_sys_connect_func(fd, address, address_len);

  rpchook_t *lp = get_by_fd(fd);
  if (!lp)
    return ret;

  if (sizeof(lp->dest) >= address_len) {
    memcpy(&(lp->dest), address, (int)address_len);
  }
  if (O_NONBLOCK & lp->user_flag) {
    return ret;
  }

  if (!(ret < 0 && errno == EINPROGRESS)) {
    return ret;
  }

  // 2.wait
  int pollret = 0;
  struct pollfd pf = {0};

  for (int i = 0; i < 3; i++) // 25s * 3 = 75s
  {
    memset(&pf, 0, sizeof(pf));
    pf.fd = fd;
    pf.events = (POLLOUT | POLLERR | POLLHUP);

    pollret = poll(&pf, 1, 25000);

    if (1 == pollret) {
      break;
    }
  }

  if (pf.revents & POLLOUT) // connect succ
  {
    // 3.check getsockopt ret
    int err = 0;
    socklen_t errlen = sizeof(err);
    ret = getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
    if (ret < 0) {
      return ret;
    } else if (err != 0) {
      errno = err;
      return -1;
    }
    errno = 0;
    return 0;
  }

  errno = ETIMEDOUT;
  return ret;
}

int close(int fd) {
  HOOK_SYS_FUNC(close);

  if (!co_is_enable_sys_hook()) {
    return g_sys_close_func(fd);
  }

  free_by_fd(fd);
  int ret = g_sys_close_func(fd);

  return ret;
}
ssize_t read(int fd, void *buf, size_t nbyte) {
  HOOK_SYS_FUNC(read);

  if (!co_is_enable_sys_hook()) {
    return g_sys_read_func(fd, buf, nbyte);
  }
  rpchook_t *lp = get_by_fd(fd);

  if (!lp || (O_NONBLOCK & lp->user_flag)) {
    ssize_t ret = g_sys_read_func(fd, buf, nbyte);
    return ret;
  }
  int timeout =
      (lp->read_timeout.tv_sec * 1000) + (lp->read_timeout.tv_usec / 1000);

  struct pollfd pf = {0};
  pf.fd = fd;
  pf.events = (POLLIN | POLLERR | POLLHUP);

  int pollret = poll(&pf, 1, timeout);

  ssize_t readret = g_sys_read_func(fd, (char *)buf, nbyte);

  if (readret < 0) {
    co_log_err("CO_ERR: read fd %d ret %ld errno %d poll ret %d timeout %d", fd,
               readret, errno, pollret, timeout);
  }

  return readret;
}
ssize_t write(int fd, const void *buf, size_t nbyte) {
  HOOK_SYS_FUNC(write);

  if (!co_is_enable_sys_hook()) {
    return g_sys_write_func(fd, buf, nbyte);
  }
  rpchook_t *lp = get_by_fd(fd);

  if (!lp || (O_NONBLOCK & lp->user_flag)) {
    ssize_t ret = g_sys_write_func(fd, buf, nbyte);
    return ret;
  }
  size_t wrotelen = 0;
  int timeout =
      (lp->write_timeout.tv_sec * 1000) + (lp->write_timeout.tv_usec / 1000);

  ssize_t writeret =
      g_sys_write_func(fd, (const char *)buf + wrotelen, nbyte - wrotelen);

  if (writeret == 0) {
    return writeret;
  }

  if (writeret > 0) {
    wrotelen += writeret;
  }
  while (wrotelen < nbyte) {

    struct pollfd pf = {0};
    pf.fd = fd;
    pf.events = (POLLOUT | POLLERR | POLLHUP);
    poll(&pf, 1, timeout);

    writeret =
        g_sys_write_func(fd, (const char *)buf + wrotelen, nbyte - wrotelen);

    if (writeret <= 0) {
      break;
    }
    wrotelen += writeret;
  }
  if (writeret <= 0 && wrotelen == 0) {
    return writeret;
  }
  return wrotelen;
}

ssize_t sendto(int socket, const void *message, size_t length, int flags,
               const struct sockaddr *dest_addr, socklen_t dest_len) {
  /*
          1.no enable sys call ? sys
          2.( !lp || lp is non block ) ? sys
          3.try
          4.wait
          5.try
  */
  HOOK_SYS_FUNC(sendto);
  if (!co_is_enable_sys_hook()) {
    return g_sys_sendto_func(socket, message, length, flags, dest_addr,
                             dest_len);
  }

  rpchook_t *lp = get_by_fd(socket);
  if (!lp || (O_NONBLOCK & lp->user_flag)) {
    return g_sys_sendto_func(socket, message, length, flags, dest_addr,
                             dest_len);
  }

  ssize_t ret =
      g_sys_sendto_func(socket, message, length, flags, dest_addr, dest_len);
  if (ret < 0 && EAGAIN == errno) {
    int timeout =
        (lp->write_timeout.tv_sec * 1000) + (lp->write_timeout.tv_usec / 1000);

    struct pollfd pf = {0};
    pf.fd = socket;
    pf.events = (POLLOUT | POLLERR | POLLHUP);
    poll(&pf, 1, timeout);

    ret =
        g_sys_sendto_func(socket, message, length, flags, dest_addr, dest_len);
  }
  return ret;
}

ssize_t recvfrom(int socket, void *buffer, size_t length, int flags,
                 struct sockaddr *address, socklen_t *address_len) {
  HOOK_SYS_FUNC(recvfrom);
  if (!co_is_enable_sys_hook()) {
    return g_sys_recvfrom_func(socket, buffer, length, flags, address,
                               address_len);
  }

  rpchook_t *lp = get_by_fd(socket);
  if (!lp || (O_NONBLOCK & lp->user_flag)) {
    return g_sys_recvfrom_func(socket, buffer, length, flags, address,
                               address_len);
  }

  int timeout =
      (lp->read_timeout.tv_sec * 1000) + (lp->read_timeout.tv_usec / 1000);

  struct pollfd pf = {0};
  pf.fd = socket;
  pf.events = (POLLIN | POLLERR | POLLHUP);
  poll(&pf, 1, timeout);

  ssize_t ret =
      g_sys_recvfrom_func(socket, buffer, length, flags, address, address_len);
  return ret;
}

ssize_t send(int socket, const void *buffer, size_t length, int flags) {
  HOOK_SYS_FUNC(send);

  if (!co_is_enable_sys_hook()) {
    return g_sys_send_func(socket, buffer, length, flags);
  }
  rpchook_t *lp = get_by_fd(socket);

  if (!lp || (O_NONBLOCK & lp->user_flag)) {
    return g_sys_send_func(socket, buffer, length, flags);
  }
  size_t wrotelen = 0;
  int timeout =
      (lp->write_timeout.tv_sec * 1000) + (lp->write_timeout.tv_usec / 1000);

  ssize_t writeret = g_sys_send_func(socket, buffer, length, flags);
  if (writeret == 0) {
    return writeret;
  }

  if (writeret > 0) {
    wrotelen += writeret;
  }
  while (wrotelen < length) {

    struct pollfd pf = {0};
    pf.fd = socket;
    pf.events = (POLLOUT | POLLERR | POLLHUP);
    poll(&pf, 1, timeout);

    writeret = g_sys_send_func(socket, (const char *)buffer + wrotelen,
                               length - wrotelen, flags);

    if (writeret <= 0) {
      break;
    }
    wrotelen += writeret;
  }
  if (writeret <= 0 && wrotelen == 0) {
    return writeret;
  }
  return wrotelen;
}

ssize_t recv(int socket, void *buffer, size_t length, int flags) {
  HOOK_SYS_FUNC(recv);

  if (!co_is_enable_sys_hook()) {
    return g_sys_recv_func(socket, buffer, length, flags);
  }
  rpchook_t *lp = get_by_fd(socket);

  if (!lp || (O_NONBLOCK & lp->user_flag)) {
    return g_sys_recv_func(socket, buffer, length, flags);
  }
  int timeout =
      (lp->read_timeout.tv_sec * 1000) + (lp->read_timeout.tv_usec / 1000);

  struct pollfd pf = {0};
  pf.fd = socket;
  pf.events = (POLLIN | POLLERR | POLLHUP);

  int pollret = poll(&pf, 1, timeout);

  ssize_t readret = g_sys_recv_func(socket, buffer, length, flags);

  if (readret < 0) {
    co_log_err("CO_ERR: read fd %d ret %ld errno %d poll ret %d timeout %d",
               socket, readret, errno, pollret, timeout);
  }

  return readret;
}

namespace co {
extern int co_poll_inner(struct pollfd fds[], nfds_t nfds, int timeout,
                         int (*pollfunc)(struct pollfd[], nfds_t, int));
} // namespace co

int poll(struct pollfd fds[], nfds_t nfds, int timeout) {
  HOOK_SYS_FUNC(poll);

  if (!co_is_enable_sys_hook() || timeout == 0) {
    return g_sys_poll_func(fds, nfds, timeout);
  }
  pollfd *fds_merge = nullptr;
  nfds_t nfds_merge = 0;
  std::map<int, int> m; // fd --> idx
  std::map<int, int>::iterator it;
  if (nfds > 1) {
    fds_merge = (pollfd *)malloc(sizeof(pollfd) * nfds);
    for (size_t i = 0; i < nfds; i++) {
      if ((it = m.find(fds[i].fd)) == m.end()) {
        fds_merge[nfds_merge] = fds[i];
        m[fds[i].fd] = nfds_merge;
        nfds_merge++;
      } else {
        int j = it->second;
        fds_merge[j].events |= fds[i].events; // merge in j slot
      }
    }
  }

  int ret = 0;
  if (nfds_merge == nfds || nfds == 1) {
    ret = co_poll_inner(fds, nfds, timeout, g_sys_poll_func);
  } else {
    ret = co_poll_inner(fds_merge, nfds_merge, timeout, g_sys_poll_func);
    if (ret > 0) {
      for (size_t i = 0; i < nfds; i++) {
        it = m.find(fds[i].fd);
        if (it != m.end()) {
          int j = it->second;
          fds[i].revents = fds_merge[j].revents & fds[i].events;
        }
      }
    }
  }
  free(fds_merge);
  return ret;
}
int setsockopt(int fd, int level, int option_name, const void *option_value,
               socklen_t option_len) {
  HOOK_SYS_FUNC(setsockopt);

  if (!co_is_enable_sys_hook()) {
    return g_sys_setsockopt_func(fd, level, option_name, option_value,
                                 option_len);
  }
  rpchook_t *lp = get_by_fd(fd);

  if (lp && SOL_SOCKET == level) {
    struct timeval *val = (struct timeval *)option_value;
    if (SO_RCVTIMEO == option_name) {
      memcpy(&lp->read_timeout, val, sizeof(*val));
    } else if (SO_SNDTIMEO == option_name) {
      memcpy(&lp->write_timeout, val, sizeof(*val));
    }
  }
  return g_sys_setsockopt_func(fd, level, option_name, option_value,
                               option_len);
}

int fcntl(int fildes, int cmd, ...) {
  HOOK_SYS_FUNC(fcntl);

  if (fildes < 0) {
    return __LINE__;
  }

  va_list arg_list;
  va_start(arg_list, cmd);

  int ret = -1;
  rpchook_t *lp = get_by_fd(fildes);
  switch (cmd) {
  case F_DUPFD: {
    int param = va_arg(arg_list, int);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }
  case F_GETFD: {
    ret = g_sys_fcntl_func(fildes, cmd);
    break;
  }
  case F_SETFD: {
    int param = va_arg(arg_list, int);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }
  case F_GETFL: {
    ret = g_sys_fcntl_func(fildes, cmd);
    if (lp && !(lp->user_flag & O_NONBLOCK)) {
      ret = ret & (~O_NONBLOCK);
    }
    break;
  }
  case F_SETFL: {
    int param = va_arg(arg_list, int);
    int flag = param;
    if (co_is_enable_sys_hook() && lp) {
      flag |= O_NONBLOCK;
    }
    ret = g_sys_fcntl_func(fildes, cmd, flag);
    if (0 == ret && lp) {
      lp->user_flag = param;
    }
    break;
  }
  case F_GETOWN: {
    ret = g_sys_fcntl_func(fildes, cmd);
    break;
  }
  case F_SETOWN: {
    int param = va_arg(arg_list, int);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }
  case F_GETLK: {
    struct flock *param = va_arg(arg_list, struct flock *);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }
  case F_SETLK: {
    struct flock *param = va_arg(arg_list, struct flock *);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }
  case F_SETLKW: {
    struct flock *param = va_arg(arg_list, struct flock *);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }
  }

  va_end(arg_list);

  return ret;
}

struct stCoSysEnv_t {
  char *name;
  char *value;
};
struct stCoSysEnvArr_t {
  stCoSysEnv_t *data;
  size_t cnt;
};
static stCoSysEnvArr_t *dup_co_sysenv_arr(stCoSysEnvArr_t *arr) {
  stCoSysEnvArr_t *lp = (stCoSysEnvArr_t *)calloc(sizeof(stCoSysEnvArr_t), 1);
  if (arr->cnt) {
    lp->data = (stCoSysEnv_t *)calloc(sizeof(stCoSysEnv_t) * arr->cnt, 1);
    lp->cnt = arr->cnt;
    memcpy(lp->data, arr->data, sizeof(stCoSysEnv_t) * arr->cnt);
  }
  return lp;
}

static int co_sysenv_comp(const void *a, const void *b) {
  return strcmp(((stCoSysEnv_t *)a)->name, ((stCoSysEnv_t *)b)->name);
}
static stCoSysEnvArr_t g_co_sysenv = {0};

namespace co {
void co_set_env_list(const char *name[], size_t cnt) {
  if (g_co_sysenv.data) {
    return;
  }
  g_co_sysenv.data = (stCoSysEnv_t *)calloc(1, sizeof(stCoSysEnv_t) * cnt);

  for (size_t i = 0; i < cnt; i++) {
    if (name[i] && name[i][0]) {
      g_co_sysenv.data[g_co_sysenv.cnt++].name = strdup(name[i]);
    }
  }
  if (g_co_sysenv.cnt > 1) {
    qsort(g_co_sysenv.data, g_co_sysenv.cnt, sizeof(stCoSysEnv_t),
          co_sysenv_comp);
    stCoSysEnv_t *lp = g_co_sysenv.data;
    stCoSysEnv_t *lq = g_co_sysenv.data + 1;
    for (size_t i = 1; i < g_co_sysenv.cnt; i++) {
      if (strcmp(lp->name, lq->name)) {
        ++lp;
        if (lq != lp) {
          *lp = *lq;
        }
      }
      ++lq;
    }
    g_co_sysenv.cnt = lp - g_co_sysenv.data + 1;
  }
}
} // namespace co

int setenv(const char *n, const char *value, int overwrite) {
  HOOK_SYS_FUNC(setenv)
  if (co_is_enable_sys_hook() && g_co_sysenv.data) {
    Coroutine *self = co_self();
    if (self) {
      if (!self->GetSysEnvs()) {
        self->GetSysEnvs() = dup_co_sysenv_arr(&g_co_sysenv);
      }
      stCoSysEnvArr_t *arr = (stCoSysEnvArr_t *)(self->GetSysEnvs());

      stCoSysEnv_t name = {(char *)n, 0};

      stCoSysEnv_t *e = (stCoSysEnv_t *)bsearch(&name, arr->data, arr->cnt,
                                                sizeof(name), co_sysenv_comp);

      if (e) {
        if (overwrite || !e->value) {
          if (e->value)
            free(e->value);
          assert(value != nullptr);
          e->value = strdup(value);
          // e->value = value != nullptr ? strdup( value ) : nullptr;
        }
        return 0;
      }
    }
  }
  return g_sys_setenv_func(n, value, overwrite);
}
int unsetenv(const char *n) {
  HOOK_SYS_FUNC(unsetenv)
  if (co_is_enable_sys_hook() && g_co_sysenv.data) {
    Coroutine *self = co_self();
    if (self) {
      if (!self->GetSysEnvs()) {
        self->GetSysEnvs() = dup_co_sysenv_arr(&g_co_sysenv);
      }
      stCoSysEnvArr_t *arr = (stCoSysEnvArr_t *)(self->GetSysEnvs());

      stCoSysEnv_t name = {(char *)n, 0};

      stCoSysEnv_t *e = (stCoSysEnv_t *)bsearch(&name, arr->data, arr->cnt,
                                                sizeof(name), co_sysenv_comp);

      if (e) {
        if (e->value) {
          free(e->value);
          e->value = 0;
        }
        return 0;
      }
    }
  }
  return g_sys_unsetenv_func(n);
}
char *getenv(const char *n) {
  HOOK_SYS_FUNC(getenv)
  if (co_is_enable_sys_hook() && g_co_sysenv.data) {
    Coroutine *self = co_self();

    stCoSysEnv_t name = {(char *)n, 0};

    if (!self->GetSysEnvs()) {
      self->GetSysEnvs() = dup_co_sysenv_arr(&g_co_sysenv);
    }
    stCoSysEnvArr_t *arr = (stCoSysEnvArr_t *)(self->GetSysEnvs());

    stCoSysEnv_t *e = (stCoSysEnv_t *)bsearch(&name, arr->data, arr->cnt,
                                              sizeof(name), co_sysenv_comp);

    if (e) {
      return e->value;
    }
  }
  return g_sys_getenv_func(n);
}

extern "C" {
int __poll(struct pollfd fds[], nfds_t nfds, int timeout) {
  return poll(fds, nfds, timeout);
}
}

namespace co {
void co_enable_hook_sys() // 这函数必须在这里,否则本文件会被忽略！！！
{
  Coroutine *co = co_self();
  if (co) {
    co->EnableHook();
  }
}
} // namespace co
