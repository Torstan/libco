#pragma once

namespace co {

class ThreadWorker {
public:
  explicit ThreadWorker(int idx);
  ~ThreadWorker() {}
  void run_loop(bool forever = true);
};

} // namespace co
