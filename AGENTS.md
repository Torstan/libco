# Repository Guidance

## Process, Thread, and Coroutine Lifecycle

- The supported startup order is: complete any `fork()` calls while the process
  is single-threaded, create worker threads as needed, then initialize each
  thread's own `ThreadEnv` and create/run coroutines there. A single-threaded
  application may use its main thread as the execution thread.
- All `fork()` calls must precede worker-thread creation. Merely avoiding
  coroutine initialization before `fork()` is insufficient: `close()` and
  `fcntl()` can acquire the global fd metadata mutex even when hooks are disabled
  and no coroutine environment exists.
- Once any thread has initialized a coroutine environment, `fork()` is unsupported
  throughout that process. Disabling hooks, finishing coroutines, or terminating
  worker threads does not restore fork support.
- This is a caller contract, not a runtime check. Preserve fd metadata locking;
  do not add PID checks, `pthread_atfork` handlers, child-process recovery, or lock
  bypasses to support excluded startup sequences unless the user explicitly
  changes this contract.
- Tests that use `fork()` must fork from a single-threaded parent that has not
  initialized a coroutine environment. Create any worker threads and coroutine
  environments independently inside the child. A reproduction that forks after
  thread creation or coroutine initialization exercises an unsupported sequence.
