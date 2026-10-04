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
#include <memory>
#include <poll.h>

namespace co {
// Events use poll masks. Native event layout and registration storage stay private.
struct IoEvent {
  short events{0};
  void *data{nullptr};
};
class EpollCtx {
public:
  static constexpr int MAX_EVENTS = 1024 * 10;
  EpollCtx();
  ~EpollCtx();
  int wait(int timeout_ms = 1);
  int add(int fd, const IoEvent *event);
  int del(int fd, const IoEvent *event);
  int mod(int fd, const IoEvent *event);
  IoEvent event(int index) const;
  int fd() const;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
EpollCtx *co_get_epoll_ct();
} // namespace co
