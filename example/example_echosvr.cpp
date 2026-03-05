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

#include <signal.h>
#include <stack>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#include <arpa/inet.h>
#include <atomic>
#include <errno.h>
#include <fcntl.h>
#include <list>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifdef __FreeBSD__
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#endif

using namespace std;
struct task_t {
  Coroutine *co;
  int fd;
};

static thread_local stack<task_t *> g_readwrite;
static thread_local int g_listen_fd = -1;
static int SetNonBlock(int iSock) {
  int iFlags;

  iFlags = fcntl(iSock, F_GETFL, 0);
  iFlags |= O_NONBLOCK;
  iFlags |= O_NDELAY;
  int ret = fcntl(iSock, F_SETFL, iFlags);
  return ret;
}

static void *readwrite_routine(void *arg) {

  co_enable_hook_sys();

  task_t *co = (task_t *)arg;
  char buf[1024 * 16];
  for (;;) {
    if (-1 == co->fd) {
      g_readwrite.push(co);
      co_yield_ct();
      continue;
    }

    int fd = co->fd;
    co->fd = -1;

    for (;;) {
      struct pollfd pf = {0};
      pf.fd = fd;
      pf.events = (POLLIN | POLLERR | POLLHUP);
      co_poll(&pf, 1, 1000);

      int ret = read(fd, buf, sizeof(buf));
      if (ret > 0) {
        ret = write(fd, buf, ret);
      }
      if (ret > 0 || (-1 == ret && EAGAIN == errno)) {
        continue;
      }
      close(fd);
      break;
    }
  }
  return 0;
}

class Worker {
private:
  int worker_id_;
  std::mutex mutex_;
  std::list<int> pending_fds_;
  std::atomic<bool> has_pending_fds_{false};

public:
  Worker(int worker_id) : worker_id_(worker_id) {}
  void dispatch_fd(int fd) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_fds_.push_back(fd);
    has_pending_fds_.store(true, std::memory_order_relaxed);
  }

  int reap_fds() {
    // 本函数在co_eventloop时调用，不可发生切换
    if (g_readwrite.empty())
      return 0;

    if (has_pending_fds_.load(std::memory_order_relaxed)) {
      std::list<int> pending_fds;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_fds.swap(pending_fds_);
        has_pending_fds_.store(false, std::memory_order_relaxed);
      }
      if (pending_fds.size() == 0)
        return 0;

      while (!pending_fds.empty() && !g_readwrite.empty()) {
        int fd = pending_fds.front();
        printf("fd %d on work id %d\n", fd, worker_id_);
        pending_fds.pop_front();
        task_t *co = g_readwrite.top();
        co->fd = fd;
        g_readwrite.pop();
        co_resume(co->co);
      }
      if (!pending_fds.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_fds_.swap(pending_fds);
        has_pending_fds_.store(true, std::memory_order_relaxed);
      }
    }
    return 0;
  }
  static int reap_fds_wrapper(void *w) {
    return (static_cast<Worker *>(w))->reap_fds();
  }

  void run(int co_cnt) {
    co_enable_hook_sys();
    for (int i = 0; i < co_cnt; i++) {
      task_t *task = (task_t *)calloc(1, sizeof(task_t));
      task->fd = -1;

      co_create(&(task->co), readwrite_routine, task);
      co_resume(task->co);
    }

    co_eventloop(Worker::reap_fds_wrapper, this);
    exit(0);
  }
};
std::vector<Worker *> g_workers;

int co_accept(int fd, struct sockaddr *addr, socklen_t *len);
static void *accept_routine(void *) {
  co_enable_hook_sys();
  printf("accept_routine\n");
  fflush(stdout);
  size_t dispatch_worker_id = 0;
  for (;;) {
    struct sockaddr_in addr; // maybe sockaddr_un;
    memset(&addr, 0, sizeof(addr));
    socklen_t len = sizeof(addr);

    int fd = co_accept(g_listen_fd, (struct sockaddr *)&addr, &len);
    if (fd < 0) {
      struct pollfd pf = {0};
      pf.fd = g_listen_fd;
      pf.events = (POLLIN | POLLERR | POLLHUP);
      co_poll(&pf, 1, 1000);
      continue;
    }
    SetNonBlock(fd);
    // dispatch fd
    size_t worker_sz = g_workers.size();
    printf("dispatch fd %d to worker_id:%lu\n", fd, dispatch_worker_id);
    g_workers[dispatch_worker_id]->dispatch_fd(fd);
    dispatch_worker_id = (dispatch_worker_id + 1) % worker_sz;
  }
  return 0;
}

static void SetAddr(const char *pszIP, const unsigned short shPort,
                    struct sockaddr_in &addr) {
  bzero(&addr, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(shPort);
  int nIP = 0;
  if (!pszIP || '\0' == *pszIP || 0 == strcmp(pszIP, "0") ||
      0 == strcmp(pszIP, "0.0.0.0") || 0 == strcmp(pszIP, "*")) {
    nIP = htonl(INADDR_ANY);
  } else {
    nIP = inet_addr(pszIP);
  }
  addr.sin_addr.s_addr = nIP;
}

static int CreateTcpSocket(const unsigned short shPort /* = 0 */,
                           const char *pszIP /* = "*" */,
                           bool bReuse /* = false */) {
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd >= 0) {
    if (shPort != 0) {
      if (bReuse) {
        int nReuseAddr = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &nReuseAddr,
                   sizeof(nReuseAddr));
      }
      struct sockaddr_in addr;
      SetAddr(pszIP, shPort, addr);
      int ret = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
      if (ret != 0) {
        close(fd);
        return -1;
      }
    }
  }
  return fd;
}

int main(int argc, char *argv[]) {
  struct sigaction sa;
  sa.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sa, nullptr);
  if (argc < 5) {
    printf("Usage:\n"
           "example_echosvr [IP] [PORT] [TASK_COUNT] [PROCESS_COUNT]\n"
           "example_echosvr [IP] [PORT] [TASK_COUNT] [PROCESS_COUNT] -d   # "
           "daemonize mode\n");
    return -1;
  }
  const char *ip = argv[1];
  int port = atoi(argv[2]);
  int cnt = atoi(argv[3]);
  int proccnt = atoi(argv[4]);
  // bool deamonize = argc >= 6 && strcmp(argv[5], "-d") == 0;

  g_listen_fd = CreateTcpSocket(port, ip, true);
  listen(g_listen_fd, 1024);
  if (g_listen_fd == -1) {
    printf("Port %d is in use\n", port);
    return -1;
  }
  printf("listen %d %s:%d\n", g_listen_fd, ip, port);

  SetNonBlock(g_listen_fd);
  g_workers.reserve(proccnt);

  for (int k = 0; k < proccnt; k++) {
    Worker *w = new Worker(k);
    g_workers.push_back(w);
    std::thread([w, cnt]() { w->run(cnt); }).detach();
  }

  Coroutine *accept_co = nullptr;
  co_create(&accept_co, accept_routine, 0);
  co_resume(accept_co);

  co_eventloop(0, 0);

  exit(0);

  return 0;
}
