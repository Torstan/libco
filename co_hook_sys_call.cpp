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
#include <stdexcept>
#include <string.h>

#include "co_routine.h"
#include "internal/poll.h"
#include "internal/hook_state.h"
#include "internal/util.h"
#include <mutex>
#include <time.h>

using namespace co;

struct rpchook_t {
  int user_flag;

  struct timeval read_timeout;
  struct timeval write_timeout;
};
static rpchook_t *g_rpchook_socket_fd[102400] = {0};
static std::mutex g_hook_fd_mutex;

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

static constexpr int kMaxHookFdCount =
    sizeof(g_rpchook_socket_fd) / sizeof(g_rpchook_socket_fd[0]);

#define HOOK_SYS_FUNC(name)                                                    \
  if (!g_sys_##name##_func) {                                                  \
    g_sys_##name##_func = (name##_pfn_t)dlsym(RTLD_NEXT, #name);               \
  }

// Only use a borrowed entry while holding g_hook_fd_mutex.
static rpchook_t *get_by_fd_locked(int fd) {
  return fd >= 0 && fd < kMaxHookFdCount ? g_rpchook_socket_fd[fd] : nullptr;
}

static bool snapshot_by_fd(int fd, rpchook_t *snapshot) {
  std::lock_guard<std::mutex> lock(g_hook_fd_mutex);
  rpchook_t *entry = get_by_fd_locked(fd);
  if (!entry) return false;
  *snapshot = *entry;
  return true;
}

static bool alloc_by_fd(int fd, const rpchook_t& value) {
  if (fd < 0 || fd >= kMaxHookFdCount) return false;
  rpchook_t *entry = (rpchook_t *)malloc(sizeof(rpchook_t));
  if (!entry) return false;
  *entry = value;
  std::lock_guard<std::mutex> lock(g_hook_fd_mutex);
  free(g_rpchook_socket_fd[fd]);
  g_rpchook_socket_fd[fd] = entry;
  return true;
}

static void free_by_fd(int fd) {
  std::lock_guard<std::mutex> lock(g_hook_fd_mutex);
  if (fd >= 0 && fd < kMaxHookFdCount) {
    free(g_rpchook_socket_fd[fd]);
    g_rpchook_socket_fd[fd] = nullptr;
  }
}

static inline int timeval_to_ms(const struct timeval &timeout) {
  return (timeout.tv_sec * 1000) + (timeout.tv_usec / 1000);
}

static inline bool should_bypass_hook(int fd, rpchook_t *snapshot) {
  return !co_is_enable_sys_hook() || !snapshot_by_fd(fd, snapshot) ||
         (snapshot->user_flag & O_NONBLOCK);
}

static int wait_for_fd(int fd, short events, int timeout_ms) {
  struct pollfd pf = {0};
  pf.fd = fd;
  pf.events = events;
  return poll(&pf, 1, timeout_ms);
}

template <typename WriteOnce>
static ssize_t write_with_retry(int fd, const void *buffer, size_t length,
                                int timeout_ms, WriteOnce write_once) {
  size_t written = 0;
  ssize_t write_ret =
      write_once((const char *)buffer + written, length - written);
  if (write_ret == 0) {
    return write_ret;
  }
  if (write_ret > 0) {
    written += write_ret;
  }
  while (written < length) {
    wait_for_fd(fd, POLLOUT | POLLERR | POLLHUP, timeout_ms);
    write_ret = write_once((const char *)buffer + written, length - written);
    if (write_ret <= 0) {
      break;
    }
    written += write_ret;
  }
  if (write_ret <= 0 && written == 0) {
    return write_ret;
  }
  return written;
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

  rpchook_t state{};
  state.read_timeout.tv_sec = state.write_timeout.tv_sec = 1;
  if (!alloc_by_fd(fd, state)) {
    g_sys_close_func(fd);
    errno = ENOMEM;
    return -1;
  }

  fcntl(fd, F_SETFL, g_sys_fcntl_func(fd, F_GETFL, 0));

  return fd;
}

int co_accept(int fd, struct sockaddr *addr, socklen_t *len) {
  int cli = accept(fd, addr, len);
  if (cli < 0) {
    return cli;
  }

  rpchook_t state{};
  state.read_timeout.tv_sec = state.write_timeout.tv_sec = 1;
  rpchook_t parent{};
  if (snapshot_by_fd(fd, &parent)) {
    state.read_timeout = parent.read_timeout;
    state.write_timeout = parent.write_timeout;
  }
  if (!alloc_by_fd(cli, state)) {
    g_sys_close_func(cli);
    errno = ENOMEM;
    return -1;
  }

  fcntl(cli, F_SETFL, g_sys_fcntl_func(cli, F_GETFL, 0));
  return cli;
}
int connect(int fd, const struct sockaddr *address, socklen_t address_len) {
  HOOK_SYS_FUNC(connect);

  if (!co_is_enable_sys_hook()) {
    return g_sys_connect_func(fd, address, address_len);
  }

  // 1.sys call
  int ret = g_sys_connect_func(fd, address, address_len);

  rpchook_t state{};
  if (!snapshot_by_fd(fd, &state) || (state.user_flag & O_NONBLOCK)) {
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
    if (pollret < 0 && errno != EINTR) {
      return -1;
    }
  }

  if (pollret < 0) return -1;
  // Error/hangup readiness also carries the connection result in SO_ERROR.
  if (pollret > 0)
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
  return -1;
}

int close(int fd) {
  HOOK_SYS_FUNC(close);

  // Descriptor metadata must be retired even when coroutine hooks are disabled.
  free_by_fd(fd);
  return g_sys_close_func(fd);
}
ssize_t read(int fd, void *buf, size_t nbyte) {
  HOOK_SYS_FUNC(read);

  rpchook_t snapshot{};
  rpchook_t *lp = &snapshot;
  if (should_bypass_hook(fd, lp)) {
    ssize_t ret = g_sys_read_func(fd, buf, nbyte);
    return ret;
  }
  int timeout = timeval_to_ms(lp->read_timeout);

  int pollret = wait_for_fd(fd, POLLIN | POLLERR | POLLHUP, timeout);

  ssize_t readret = g_sys_read_func(fd, (char *)buf, nbyte);

  if (readret < 0) {
    co_log_err("CO_ERR: read fd %d ret %ld errno %d poll ret %d timeout %d", fd,
               readret, errno, pollret, timeout);
  }

  return readret;
}
ssize_t write(int fd, const void *buf, size_t nbyte) {
  HOOK_SYS_FUNC(write);

  rpchook_t snapshot{};
  rpchook_t *lp = &snapshot;
  if (should_bypass_hook(fd, lp)) {
    ssize_t ret = g_sys_write_func(fd, buf, nbyte);
    return ret;
  }
  int timeout = timeval_to_ms(lp->write_timeout);

  return write_with_retry(fd, buf, nbyte, timeout,
                          [fd](const char *buffer, size_t length) {
                            return g_sys_write_func(fd, buffer, length);
                          });
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

  rpchook_t snapshot{};
  rpchook_t *lp = &snapshot;
  if (should_bypass_hook(socket, lp)) {
    return g_sys_sendto_func(socket, message, length, flags, dest_addr,
                             dest_len);
  }

  ssize_t ret =
      g_sys_sendto_func(socket, message, length, flags, dest_addr, dest_len);
  if (ret < 0 && EAGAIN == errno) {
    int timeout = timeval_to_ms(lp->write_timeout);

    wait_for_fd(socket, POLLOUT | POLLERR | POLLHUP, timeout);

    ret =
        g_sys_sendto_func(socket, message, length, flags, dest_addr, dest_len);
  }
  return ret;
}

ssize_t recvfrom(int socket, void *buffer, size_t length, int flags,
                 struct sockaddr *address, socklen_t *address_len) {
  HOOK_SYS_FUNC(recvfrom);

  rpchook_t snapshot{};
  rpchook_t *lp = &snapshot;
  if (should_bypass_hook(socket, lp)) {
    return g_sys_recvfrom_func(socket, buffer, length, flags, address,
                               address_len);
  }

  int timeout = timeval_to_ms(lp->read_timeout);

  wait_for_fd(socket, POLLIN | POLLERR | POLLHUP, timeout);

  ssize_t ret =
      g_sys_recvfrom_func(socket, buffer, length, flags, address, address_len);
  return ret;
}

ssize_t send(int socket, const void *buffer, size_t length, int flags) {
  HOOK_SYS_FUNC(send);

  rpchook_t snapshot{};
  rpchook_t *lp = &snapshot;
  if (should_bypass_hook(socket, lp)) {
    return g_sys_send_func(socket, buffer, length, flags);
  }
  int timeout = timeval_to_ms(lp->write_timeout);

  return write_with_retry(socket, buffer, length, timeout,
                          [socket, flags](const char *buffer, size_t length) {
                            return g_sys_send_func(socket, buffer, length,
                                                   flags);
                          });
}

ssize_t recv(int socket, void *buffer, size_t length, int flags) {
  HOOK_SYS_FUNC(recv);

  rpchook_t snapshot{};
  rpchook_t *lp = &snapshot;
  if (should_bypass_hook(socket, lp)) {
    return g_sys_recv_func(socket, buffer, length, flags);
  }
  int timeout = timeval_to_ms(lp->read_timeout);

  int pollret = wait_for_fd(socket, POLLIN | POLLERR | POLLHUP, timeout);

  ssize_t readret = g_sys_recv_func(socket, buffer, length, flags);

  if (readret < 0) {
    co_log_err("CO_ERR: read fd %d ret %ld errno %d poll ret %d timeout %d",
               socket, readret, errno, pollret, timeout);
  }

  return readret;
}

int poll(struct pollfd fds[], nfds_t nfds, int timeout) {
  HOOK_SYS_FUNC(poll);
  if (!co_is_enable_sys_hook() || timeout == 0) {
    return g_sys_poll_func(fds, nfds, timeout);
  }
  return co::detail::PollWait(fds, nfds, timeout, g_sys_poll_func);
}
int setsockopt(int fd, int level, int option_name, const void *option_value,
               socklen_t option_len) {
  HOOK_SYS_FUNC(setsockopt);

  if (!co_is_enable_sys_hook()) {
    return g_sys_setsockopt_func(fd, level, option_name, option_value,
                                 option_len);
  }
  // Serialize the native update with its cached value and metadata retirement.
  // This native control call cannot yield through our coroutine hooks.
  std::lock_guard<std::mutex> lock(g_hook_fd_mutex);
  // Let the kernel validate the arguments before reading or caching them.
  int ret = g_sys_setsockopt_func(fd, level, option_name, option_value,
                                 option_len);
  if (ret != 0) {
    return ret;
  }
  rpchook_t *lp = get_by_fd_locked(fd);

  if (lp && SOL_SOCKET == level && option_value &&
      option_len >= sizeof(struct timeval)) {
    const struct timeval *val = (const struct timeval *)option_value;
    if (SO_RCVTIMEO == option_name) {
      memcpy(&lp->read_timeout, val, sizeof(*val));
    } else if (SO_SNDTIMEO == option_name) {
      memcpy(&lp->write_timeout, val, sizeof(*val));
    }
  }
  return ret;
}

static int handle_f_getfl(int fd, rpchook_t *hook) {
  int ret = g_sys_fcntl_func(fd, F_GETFL);
  if (ret >= 0 && hook && !(hook->user_flag & O_NONBLOCK)) {
    ret = ret & (~O_NONBLOCK);
  }
  return ret;
}

static int handle_f_setfl(int fd, int param, rpchook_t *hook) {
  int flag = param;
  if (co_is_enable_sys_hook() && hook) {
    flag |= O_NONBLOCK;
  }
  int ret = g_sys_fcntl_func(fd, F_SETFL, flag);
  if (ret == 0 && hook) {
    hook->user_flag = param;
  }
  return ret;
}

int fcntl(int fildes, int cmd, ...) {
  HOOK_SYS_FUNC(fcntl);

  if (fildes < 0) {
    errno = EBADF;
    return -1;
  }

  va_list arg_list;
  va_start(arg_list, cmd);

  int ret = -1;
  switch (cmd) {
  case F_DUPFD: {
    int param = va_arg(arg_list, int);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }
#ifdef F_DUPFD_CLOEXEC
  case F_DUPFD_CLOEXEC: {
    int param = va_arg(arg_list, int);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }
#endif
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
    std::lock_guard<std::mutex> lock(g_hook_fd_mutex);
    ret = handle_f_getfl(fildes, get_by_fd_locked(fildes));
    break;
  }
  case F_SETFL: {
    int param = va_arg(arg_list, int);
    std::lock_guard<std::mutex> lock(g_hook_fd_mutex);
    ret = handle_f_setfl(fildes, param, get_by_fd_locked(fildes));
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
#ifdef F_GETSIG
  case F_GETSIG: {
    ret = g_sys_fcntl_func(fildes, cmd);
    break;
  }
#endif
#ifdef F_SETSIG
  case F_SETSIG: {
    int param = va_arg(arg_list, int);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }
#endif
#ifdef F_GETLEASE
  case F_GETLEASE: {
    ret = g_sys_fcntl_func(fildes, cmd);
    break;
  }
#endif
#ifdef F_SETLEASE
  case F_SETLEASE: {
    int param = va_arg(arg_list, int);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }
#endif
#ifdef F_NOTIFY
  case F_NOTIFY: {
    int param = va_arg(arg_list, int);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }
#endif
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
#ifdef F_OFD_GETLK
  case F_OFD_GETLK: {
    struct flock *param = va_arg(arg_list, struct flock *);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }
#endif
#ifdef F_OFD_SETLK
  case F_OFD_SETLK: {
    struct flock *param = va_arg(arg_list, struct flock *);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }
#endif
#ifdef F_OFD_SETLKW
  case F_OFD_SETLKW: {
    struct flock *param = va_arg(arg_list, struct flock *);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }
#endif
#ifdef F_GETOWN_EX
  case F_GETOWN_EX: {
    struct f_owner_ex *param = va_arg(arg_list, struct f_owner_ex *);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }
#endif
#ifdef F_SETOWN_EX
  case F_SETOWN_EX: {
    struct f_owner_ex *param = va_arg(arg_list, struct f_owner_ex *);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }
#endif
#ifdef F_GETPIPE_SZ
  case F_GETPIPE_SZ: {
    ret = g_sys_fcntl_func(fildes, cmd);
    break;
  }
#endif
#ifdef F_SETPIPE_SZ
  case F_SETPIPE_SZ: {
    int param = va_arg(arg_list, int);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }
#endif
#ifdef F_ADD_SEALS
  case F_ADD_SEALS: {
    int param = va_arg(arg_list, int);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }
#endif
#ifdef F_GET_SEALS
  case F_GET_SEALS: {
    ret = g_sys_fcntl_func(fildes, cmd);
    break;
  }
#endif
#ifdef F_GET_RW_HINT
  case F_GET_RW_HINT: {
    uint64_t *param = va_arg(arg_list, uint64_t *);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }
#endif
#ifdef F_SET_RW_HINT
  case F_SET_RW_HINT: {
    uint64_t *param = va_arg(arg_list, uint64_t *);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }
#endif
#ifdef F_GET_FILE_RW_HINT
  case F_GET_FILE_RW_HINT: {
    uint64_t *param = va_arg(arg_list, uint64_t *);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }
#endif
#ifdef F_SET_FILE_RW_HINT
  case F_SET_FILE_RW_HINT: {
    uint64_t *param = va_arg(arg_list, uint64_t *);
    ret = g_sys_fcntl_func(fildes, cmd, param);
    break;
  }
#endif
  default:
    errno = EINVAL;
    ret = -1;
    break;
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

namespace co {
void co_cleanup_sys_envs(void *envs) {
  stCoSysEnvArr_t *arr = (stCoSysEnvArr_t *)envs;
  if (!arr) {
    return;
  }
  for (size_t i = 0; i < arr->cnt; ++i) {
    if (arr->data[i].value) {
      free(arr->data[i].value);
      arr->data[i].value = nullptr;
    }
  }
  free(arr->data);
  free(arr);
}
} // namespace co

static int co_sysenv_comp(const void *a, const void *b) {
  return strcmp(((stCoSysEnv_t *)a)->name, ((stCoSysEnv_t *)b)->name);
}
static stCoSysEnvArr_t g_co_sysenv = {0};

static stCoSysEnv_t *find_coroutine_env(const char *name, bool create) {
  if (!co_is_enable_sys_hook() || !g_co_sysenv.data) {
    return nullptr;
  }
  Coroutine *self = co_self();
  if (!self) {
    return nullptr;
  }
  if (create && !co::detail::CoroutineEnvs(*self)) {
    co::detail::CoroutineEnvs(*self) = dup_co_sysenv_arr(&g_co_sysenv);
  }
  if (!co::detail::CoroutineEnvs(*self)) {
    return nullptr;
  }
  stCoSysEnvArr_t *arr = (stCoSysEnvArr_t *)(co::detail::CoroutineEnvs(*self));
  stCoSysEnv_t key = {(char *)name, 0};
  return (stCoSysEnv_t *)bsearch(&key, arr->data, arr->cnt, sizeof(key),
                                 co_sysenv_comp);
}

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
  stCoSysEnv_t *e = find_coroutine_env(n, true);
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
  return g_sys_setenv_func(n, value, overwrite);
}
int unsetenv(const char *n) {
  HOOK_SYS_FUNC(unsetenv)
  stCoSysEnv_t *e = find_coroutine_env(n, true);
  if (e) {
    if (e->value) {
      free(e->value);
      e->value = 0;
    }
    return 0;
  }
  return g_sys_unsetenv_func(n);
}
char *getenv(const char *n) {
  HOOK_SYS_FUNC(getenv)
  stCoSysEnv_t *e = find_coroutine_env(n, true);
  if (e) {
    return e->value;
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
