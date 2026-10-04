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

## Runtime Architecture and Ownership

Keep result storage, execution transitions, waiting, and platform mechanisms under
their respective owners. `ThreadEnv` owns scheduling and wait completion; the
other modules must not maintain competing execution state. Apply KISS/YAGNI:
reuse the existing mechanisms and common public APIs, and add abstractions only
when a concrete requirement justifies them.

| Module | Files | Responsibility and boundary |
| --- | --- | --- |
| `Coroutine` | `co_routine.h/.cpp` | Represents one execution and owns its function, stack, execution flags, and environment association. Keep Context, stack allocation, and main-coroutine flags in the private `Impl`; delegate resume/yield transitions to `ThreadEnv`. |
| `ThreadEnv` | `co_routine.h/.cpp` | Owns the current coroutine, call relationships, task/ready/completion queues, timers, and I/O backend. Centralizes resume, yield, wait completion, event-loop driving, and reclamation. Queue state and ordering belong to its implementation. |
| `ThreadWorker` | `thread_worker.h/.cpp` | Drives the current thread's environment through the existing `run_loop()` entry. Holds no separate Context pointer, coroutine state, or scheduling queues. |
| `Future` / `Promise` | `co_future.h` | Owns one result or exception and its lifetime. Uses `WaitRecord` to suspend or complete a wait; must not manipulate Context chains, stacks, backend events, or execution queues. |
| `CoCond` | `co_cond.h/.cpp` | Owns condition waiters and notification order. Keep waiter nodes, timer records, and list operations in the implementation; expose `Signal`, `Broadcast`, and `Timedwait`. |
| `async` / `Task` | `co_async.h`, `task.h` | Reuses the existing callable-to-Task wrapping and `schedule()` entry. `async` connects a task to a Promise; do not introduce another executor layer. |
| `WaitRecord` | `internal/wait.h`, implementation in `co_routine.cpp` | Coordinates one suspension, idempotent completion, and cleanup before resumption. Keep queue links private and accessible only to the authorized runtime and list implementation. |
| poll semantics | `internal/poll.h/.cpp` | One `PollWait` implementation serves `co_poll` and hooked `poll`. Owns fd merging, registration/rollback, result mapping, and result counting. |
| I/O backend | `internal/io_backend.h/.cpp` | `EpollCtx` owns one kernel queue and its registration/result storage. Hide epoll/kqueue details behind poll event masks and opaque payloads. Do not place timers or coroutine ready queues here. |
| Timers | `internal/timer_queue.h` | `Timeout` owns deadlines and time-wheel placement. `TakeAll` returns only expired items and retains future deadlines internally; `Remove` encapsulates unlinking. No platform events or coroutine resumption logic. |
| Event adapters | `internal/event.h` | `IoSource` and `WaitTimer` translate I/O readiness and timer expiry into wait completion. Keep this coordination outside the backend and pure timer implementation. |
| Context / stack | `internal/context.h/.cpp`, `internal/coctx.h/.cpp`, `internal/coctx_swap.S`, `internal/stack.h` | Saves/restores platform execution state and provides stack RAII. The execution layer explicitly supplies both sides of `RoutineContext::Switch`; Context must not read Worker or scheduler state. |
| System-call hooks | `co_hook_sys_call.cpp`, `internal/hook_state.h` | Adapts native calls to coroutine behavior. Owns real-function resolution, retry rules, and fd metadata synchronization. Copy/update metadata under the mutex; do not retain borrowed entries after unlocking or suspend while holding the lock. |

## Dependencies and Public Header Boundaries

The following arrows describe runtime collaboration, not a literal include graph:

```text
async -> Task / schedule -> ThreadEnv
ThreadWorker / Coroutine lifecycle -> ThreadEnv
Future / Promise / CoCond / PollWait -> WaitRecord -> ThreadEnv
co_poll / hooked poll -> PollWait -> EpollCtx
CoCond / PollWait -> WaitTimer -> ThreadEnv's Timeout
ThreadEnv -> RoutineContext / EpollCtx / Timeout
```

- `co_routine.h`: retain common Coroutine methods and existing environment entry
  points; keep concrete Context, stack, execution flags, and queue storage in
  `.cpp` implementations. Do not restore the removed runtime accessors as public
  methods.
- `thread_worker.h`: retain the constructor, destructor, and `run_loop()` driver;
  do not expose Context switching or duplicate runtime state.
- `co_future.h`: retain template result logic and the narrow waiting interface.
  Its current dependency is `internal/wait.h -> internal/co_link.h`: `wait()`
  creates a `WaitRecord` on the stack, so it needs the complete type. The link
  fields are private, but compile-time/layout coupling remains. Do not describe
  this as a forward-declaration-only dependency or add allocation/another wrapper
  solely to remove the include.
- `co_cond.h`: expose condition methods and an opaque `Impl`; keep list nodes and
  timer layouts out of the public header.
- `co_async.h` and `task.h`: continue using the existing Task and scheduling
  interfaces. Make required includes explicit instead of relying on accidental
  transitive includes.
- `detail::CoroutineEnvs` is still declared in `co_routine.h` and returns a mutable
  reference to internal hook state. Treat it as an internal hook access point,
  not a supported application API. The `detail` namespace is a usage convention,
  not access control; do not claim that every internal access point is hidden.

## Waiting and Reclamation Contracts

- A wait registers its event/timer sources before suspension. Completion is
  idempotent and queues a suspended waiter without directly resuming it.
  `ThreadEnv` collects the whole I/O batch, processes expired timers, detaches
  sources, and only then resumes ready coroutines. Preserve this ordering to avoid
  events referring to records destroyed by an earlier resumption.
- Timer-wheel filtering and reinsertion stay inside `Timeout`; the event loop
  only converts expired records into wait completion. Timer extraction and
  completion-queue registration must not allocate memory.
- Caller-owned coroutines may be reset/freed only in their owning thread and
  only before starting or after ending. A suspended coroutine still owns a live
  stack. The main coroutine cannot be freed; resetting it is a no-op.
- Task coroutines are owned by `ThreadEnv`. Task execution and destruction must
  finish before the coroutine reports completion. `ThreadEnv::Resume` registers
  the ended coroutine after switching back to its caller's stack, without
  allocating or yielding; `Reap` performs deletion. Task destructors may suspend
  or drive nested work, so do not reclaim their active stacks.
- Future/Promise and CoCond operations are confined to one thread. A Future has
  at most one active waiter and must remain alive and unmoved while waiting;
  moving/destroying a waiting Future terminates the process. A ready Future can
  be read on the main coroutine; waiting for a pending result there throws
  `std::logic_error`. Consumed/moved-from Futures reject further result access or
  waiting. Destroying a Promise before it has produced a result completes its
  Future with a broken-promise exception.
- A CoCond must outlive its waiters. `run_loop(false)` processes runnable work;
  it does not guarantee that all pending waits have completed. There is no
  cancellation mechanism that unwinds suspended stacks at thread exit: complete
  active coroutines before their owning thread exits.
- Keep these behavior contracts close to the public APIs when updating their
  comments, particularly Future waiting and `run_loop(false)` semantics.

## Architecture Evidence and Verification

- See [runtime-architecture.md](docs/runtime-architecture.md) for the detailed
  design and evidence index. The startup contract in this file also applies to
  architecture examples and tests.
- Boundary/lifetime coverage: `test/test_runtime_boundaries.cpp`; backend failure
  paths: `test/test_io_backend.cpp`; connection results: `test/test_connect.cpp`;
  public APIs and async: `test/test_public_api.cpp`, `test/test_co_async.cpp`;
  poll and hook semantics: `test/test_co_poll.cpp`, `test/risk/`.
- For public-header changes, verify that affected headers compile independently
  and inspect their transitive includes. For waiting/scheduling changes, exercise
  completion, cleanup, suspended lifetime, and task-destructor reentrancy paths.
  Run the relevant existing tests; do not treat a passing test suite as proof that
  all implementation details are hidden.
