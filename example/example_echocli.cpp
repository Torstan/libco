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

#include "co_epoll.h"
#include "co_routine.h"
#include "co_timeout.h"
#include "thread_worker.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <stack>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <thread>
#include <time.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace std;
using namespace co;
struct stEndPoint {
  char *ip;
  unsigned short int port;
};

struct client_task_t {
  Coroutine *co;
  int fd;
  TimeoutItem io_event;
  struct epoll_event ev;
};

static int SetNonBlock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK | O_NDELAY);
}

static void OnFdReady(TimeoutItem *item) {
  client_task_t *task = static_cast<client_task_t *>(item->arg);
  co_resume(task->co);
}

static bool RegisterReadEvent(client_task_t *task) {
  TimeoutItemLink::remove(&task->io_event);
  task->io_event.arg = task;
  task->io_event.prepare_func = nullptr;
  task->io_event.process_func = OnFdReady;
  task->io_event.timeout = false;
  task->ev = {0};
  task->ev.data.ptr = &task->io_event;
  task->ev.events = (EPOLLIN | EPOLLERR | EPOLLHUP);
  return 0 == co_get_curr_thread_env()->Epoll()->add(task->fd, &task->ev);
}

static void CloseClient(client_task_t *task) {
  if (task->fd < 0) {
    return;
  }
  TimeoutItemLink::remove(&task->io_event);
  co_get_curr_thread_env()->Epoll()->del(task->fd, &task->ev);
  close(task->fd);
  task->fd = -1;
}

static bool WriteAll(int fd, const char *buf, int len) {
  int written = 0;
  while (written < len) {
    int ret = write(fd, buf + written, len - written);
    if (ret > 0) {
      written += ret;
      continue;
    }
    if (-1 == ret && EAGAIN == errno) {
      struct pollfd pf = {0};
      pf.fd = fd;
      pf.events = (POLLOUT | POLLERR | POLLHUP);
      co_poll(&pf, 1, 1000);
      continue;
    }
    return false;
  }
  return true;
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

static std::atomic<size_t> iSuccCnt{0};
static std::atomic<size_t> iFailCnt{0};
static std::atomic<size_t> iTime{0};

void inline AddSuccCnt() { iSuccCnt.fetch_add(1, std::memory_order_relaxed); }
void inline AddFailCnt() { iFailCnt.fetch_add(1, std::memory_order_relaxed); }

void PrintStat() {
  size_t now = GetTickUS();
  size_t time_old = iTime.load(std::memory_order_relaxed);
  size_t time_delta = now - time_old;
  size_t succ_cnt = iSuccCnt.load(std::memory_order_relaxed);
  if (now > time_old) {
    printf("time %lu qps %ld Succ Cnt %ld Fail Cnt %ld\n", time_old,
           succ_cnt * 1000000 / time_delta, succ_cnt,
           iFailCnt.load(std::memory_order_relaxed));
    iTime.store(now, std::memory_order_relaxed);
    iSuccCnt.store(0, std::memory_order_relaxed);
    iFailCnt.store(0, std::memory_order_relaxed);
  }
}

static void *readwrite_routine(const stEndPoint& ep) {

  co_enable_hook_sys();

  const stEndPoint *endpoint = &ep;
  char str[8] = "sarlmol";
  char buf[1024 * 16];
  client_task_t task = {};
  task.co = co_self();
  task.fd = -1;
  int ret = 0;
  for (;;) {
    if (task.fd < 0) {
      task.fd = socket(PF_INET, SOCK_STREAM, 0);
      if (task.fd < 0) {
        AddFailCnt();
        continue;
      }
      struct sockaddr_in addr;
      SetAddr(endpoint->ip, endpoint->port, addr);
      ret = connect(task.fd, (struct sockaddr *)&addr, sizeof(addr));

      if (ret < 0 && (errno == EALREADY || errno == EINPROGRESS)) {
        struct pollfd pf = {0};
        pf.fd = task.fd;
        pf.events = (POLLOUT | POLLERR | POLLHUP);
        co_poll(&pf, 1, 200);
        // check connect
        int error = 0;
        uint32_t socklen = sizeof(error);
        errno = 0;
        ret = getsockopt(task.fd, SOL_SOCKET, SO_ERROR, (void *)&error,
                         &socklen);
        if (ret == -1) {
          // printf("getsockopt ERROR ret %d %d:%s\n", ret, errno,
          // strerror(errno));
          close(task.fd);
          task.fd = -1;
          AddFailCnt();
          continue;
        }
        if (error) {
          errno = error;
          // printf("connect ERROR ret %d %d:%s\n", error, errno,
          // strerror(errno));
          close(task.fd);
          task.fd = -1;
          AddFailCnt();
          continue;
        }
      } else if (ret < 0) {
        close(task.fd);
        task.fd = -1;
        AddFailCnt();
        continue;
      }

      if (SetNonBlock(task.fd) != 0 || !RegisterReadEvent(&task)) {
        close(task.fd);
        task.fd = -1;
        AddFailCnt();
        continue;
      }
    }

    while (true) {
      if (!WriteAll(task.fd, str, sizeof(str))) {
        CloseClient(&task);
        AddFailCnt();
        break;
      }

      for (;;) {
        ret = read(task.fd, buf, sizeof(buf));
        if (ret > 0) {
          AddSuccCnt();
          break;
        }
        if (-1 == ret && EAGAIN == errno) {
          co_yield_ct();
          continue;
        }
        CloseClient(&task);
        AddFailCnt();
        break;
      }
      if (task.fd < 0) {
        break;
      }
    }
  }
  return 0;
}

int main(int argc, char *argv[]) {
  stEndPoint endpoint;
  endpoint.ip = argv[1];
  endpoint.port = atoi(argv[2]);
  int cnt = atoi(argv[3]);
  int proccnt = atoi(argv[4]);

  struct sigaction sa;
  sa.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sa, nullptr);

  for (int k = 0; k < proccnt; k++) {
    std::thread *t = new std::thread([k, cnt, ep = endpoint]() {
      ThreadWorker worker(k);
      for (int i = 0; i < cnt; i++) {
        Coroutine *co = co_create([ep]() {
          readwrite_routine(ep);
        });
        co->Resume();
      }
      worker.run_loop();
      exit(0);
    });
    t->detach();
  }

  for (;;) {
    ::usleep(1000000);
    PrintStat();
  }
  return 0;
}
// ./example_echocli 127.0.0.1 1024 100 100
