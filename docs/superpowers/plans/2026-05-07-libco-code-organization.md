# Libco Code Organization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Improve libco's organization, naming clarity, build consistency, and documentation while preserving all existing public APIs and include paths.

**Architecture:** Keep the public root headers and legacy `co_*` names intact. Align build/test/docs with the current repository, then perform behavior-preserving helper extraction inside existing `.cpp` files. Refactors stay local to their files and are verified by Makefile tests, CMake builds, and `test_co_poll` duplicate-fd coverage.

**Tech Stack:** C++17, GNU Make, CMake, POSIX sockets/poll/epoll, pthread, dl, shell test commands.

---

## File Structure

- Modify `CMakeLists.txt`: source list, existing example targets, test targets, platform link libraries.
- Modify `Makefile`: add `check` target and keep `all`, `colib`, `examples`, and `tests` behavior.
- Modify `test/Makefile`: add `test_public_api` and a `check` target that executes all test binaries.
- Modify `example/Makefile`: remove stale `example_exit` target.
- Create `test/test_public_api.cpp`: compile/link coverage for `co_accept` and `Coroutine::Reset()`.
- Modify `co_routine.h`: add `co_accept` declaration while keeping existing declarations.
- Modify `co_routine.cpp`: define `Coroutine::Reset()`, refactor `co_poll_inner()`, refactor `co_eventloop()`.
- Modify `example/example_echosvr.cpp`: remove local `co_accept` forward declaration after the public header provides it.
- Modify `README.md`: replace stale upstream claims with current project docs.
- Create `example/README.md`: example navigation and run commands.
- Modify `example/example_echocli.cpp`: fix the trailing sample command comment.
- Modify `co_cond.cpp`: extract waiter activation helper.
- Modify `thread_worker.cpp`: make task coroutine ownership clearer.
- Modify `coctx.cpp`: move duplicated `coctx_init()` outside architecture branches.
- Modify `co_hook_sys_call.cpp`: extract hook bypass, timeout, wait, write-loop, env lookup, and limited `fcntl()` helpers.

## Task 1: Build And Test Entry Points

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `Makefile`
- Modify: `test/Makefile`
- Modify: `example/Makefile`

- [ ] **Step 1: Capture current CMake failure**

Run:

```bash
cmake -S . -B /tmp/libco-cmake-before
```

Expected before implementation: FAIL with missing `example_*.cpp` sources because CMake looks in the repository root and lists examples that do not exist.

- [ ] **Step 2: Capture current missing check target**

Run:

```bash
make check
```

Expected before implementation: FAIL with no `check` target.

- [ ] **Step 3: Replace CMake source and target organization**

In `CMakeLists.txt`, replace the current source list, link setup, and example macro section with this structure. Keep the existing project name, version, and compile flag section above it.

```cmake
# Use c, c++ and asm
enable_language(CXX C ASM)

set(LIBCO_PLATFORM_LIBS pthread)
if(NOT APPLE)
    list(APPEND LIBCO_PLATFORM_LIBS dl)
endif()

# Add source files
set(SOURCE_FILES
        co_epoll.cpp
        co_cond.cpp
        thread_worker.cpp
        routine_context.cpp
        co_routine.cpp
        co_hook_sys_call.cpp
        coctx.cpp
        coctx_swap.S)

# Add static and shared library target
add_library(colib_static STATIC ${SOURCE_FILES})
add_library(colib_shared SHARED ${SOURCE_FILES})

# Set library output name
set_target_properties(colib_static PROPERTIES OUTPUT_NAME colib)
set_target_properties(colib_shared PROPERTIES OUTPUT_NAME colib)

set_target_properties(colib_static PROPERTIES CLEAN_DIRECT_OUTPUT 1)
set_target_properties(colib_shared PROPERTIES CLEAN_DIRECT_OUTPUT 1)

# Set shared library version, will generate libcolib.${LIBCO_VERSION}.so and a symbol link named libcolib.so
# For mac osx, the extension name will be .dylib
set_target_properties(colib_shared PROPERTIES VERSION ${LIBCO_VERSION} SOVERSION ${LIBCO_VERSION})

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

macro(add_example_target EXAMPLE_TARGET)
    add_executable("example_${EXAMPLE_TARGET}" "example/example_${EXAMPLE_TARGET}.cpp")
    target_link_libraries("example_${EXAMPLE_TARGET}" colib_static ${LIBCO_PLATFORM_LIBS})
endmacro(add_example_target)

add_example_target(cond)
add_example_target(echocli)
add_example_target(echosvr)
add_example_target(poll)
add_example_target(setenv)
add_example_target(thread)

enable_testing()

macro(add_libco_test TEST_TARGET)
    add_executable("${TEST_TARGET}" "test/${TEST_TARGET}.cpp")
    target_link_libraries("${TEST_TARGET}" colib_static ${LIBCO_PLATFORM_LIBS})
    add_test(NAME "${TEST_TARGET}" COMMAND "${TEST_TARGET}")
endmacro(add_libco_test)

add_libco_test(test_co_routine)
add_libco_test(test_co_async)
add_libco_test(test_co_poll)
```

- [ ] **Step 4: Add root check target**

In `Makefile`, change `.PHONY` and add `check` after `tests`.

```make
all: colib examples tests

.PHONY: all colib examples tests check clean dist

examples: libcolib.a
	$(MAKE) -C example
tests: libcolib.a
	$(MAKE) -C test
check: tests
	$(MAKE) -C test check
```

- [ ] **Step 5: Add test runner target**

In `test/Makefile`, change `.PHONY` and add `check` after the target rules.

```make
PROGS = test_co_routine test_co_async test_co_poll

all:$(PROGS)

.PHONY: all check clean dist

test_co_routine:test_co_routine.o
	$(BUILDEXE)

test_co_async:test_co_async.o
	$(BUILDEXE)

test_co_poll:test_co_poll.o
	$(BUILDEXE)

check: all
	@set -e; for prog in $(PROGS); do echo "RUN $$prog"; ./$$prog; done
```

- [ ] **Step 6: Remove stale example target**

In `example/Makefile`, delete only these two lines:

```make
example_exit:example_exit.o
	$(BUILDEXE)
```

Do not change `PROGS`; it already lists only existing examples.

- [ ] **Step 7: Verify Makefile build and test entry**

Run:

```bash
make check
```

Expected after implementation: builds the library and tests, then runs `test_co_routine`, `test_co_async`, and `test_co_poll` with exit code 0.

- [ ] **Step 8: Verify CMake configure and build**

Run:

```bash
cmake -S . -B /tmp/libco-cmake-check
cmake --build /tmp/libco-cmake-check
cd /tmp/libco-cmake-check && ctest --output-on-failure
```

Expected after implementation: configure succeeds, build succeeds, and all registered tests pass.

- [ ] **Step 9: Commit build/test entry point changes**

```bash
git add CMakeLists.txt Makefile test/Makefile example/Makefile
git commit -m "build: align make and cmake targets"
```

## Task 2: Public API Declaration And Link Coverage

**Files:**
- Create: `test/test_public_api.cpp`
- Modify: `test/Makefile`
- Modify: `CMakeLists.txt`
- Modify: `co_routine.h`
- Modify: `co_routine.cpp`
- Modify: `example/example_echosvr.cpp`

- [ ] **Step 1: Add failing public API test**

Create `test/test_public_api.cpp`:

```cpp
#include "co_routine.h"

#include <sys/socket.h>

using namespace co;

int main() {
  int (*accept_fn)(int, struct sockaddr *, socklen_t *) = co_accept;
  (void)accept_fn;

  Coroutine *routine = co_create([]() {});
  routine->Reset();
  co_free(routine);

  return 0;
}
```

- [ ] **Step 2: Register the new test in Makefile**

In `test/Makefile`, update `PROGS` and add the target:

```make
PROGS = test_co_routine test_co_async test_co_poll test_public_api
```

```make
test_public_api:test_public_api.o
	$(BUILDEXE)
```

Keep the `check` target from Task 1 unchanged so it automatically runs `test_public_api`.

- [ ] **Step 3: Register the new test in CMake**

In `CMakeLists.txt`, add this line after `add_libco_test(test_co_poll)`:

```cmake
add_libco_test(test_public_api)
```

- [ ] **Step 4: Run the public API test and verify it fails**

Run:

```bash
make -C test test_public_api
```

Expected before implementation: FAIL because `co_accept` is not declared in `co_routine.h` or because `Coroutine::Reset()` is declared but not defined at link time.

- [ ] **Step 5: Declare `co_accept` publicly**

In `co_routine.h`, add `<sys/socket.h>` next to the existing system includes:

```cpp
#include <sys/poll.h>
#include <sys/socket.h>
```

Add the public declaration near `co_poll` and `co_eventloop`:

```cpp
int co_accept(int fd, struct sockaddr *addr, socklen_t *len);
int co_poll(struct pollfd fds[], nfds_t nfds, int timeout_ms);
void co_eventloop(pfn_co_eventloop_t func, void *arg);
```

- [ ] **Step 6: Define `Coroutine::Reset()`**

In `co_routine.cpp`, add a shared stack size constant near `gCoEnvPerThread`:

```cpp
static thread_local ThreadEnv *gCoEnvPerThread = nullptr;
static constexpr int kDefaultStackSize = 256 * 1024;
```

Replace the constructor's local stack constant:

```cpp
  if (func_) {
    stack_mem_ = std::make_unique<StackMem>(kDefaultStackSize);
    routine_ctx_.InitCtx(stack_mem_->GetStackBuffer(), kDefaultStackSize);
  }
```

Add this definition after `Coroutine::Resume()`:

```cpp
void Coroutine::Reset() {
  if (!stack_mem_) {
    return;
  }
  started_ = false;
  ended_ = false;
  routine_ctx_.InitCtx(stack_mem_->GetStackBuffer(), kDefaultStackSize);
}
```

This keeps the API linkable without changing public names. It resets execution state and context for regular coroutine objects, and leaves the main coroutine unchanged because it has no stack memory.

- [ ] **Step 7: Remove local `co_accept` declaration**

In `example/example_echosvr.cpp`, delete this line:

```cpp
int co_accept(int fd, struct sockaddr *addr, socklen_t *len);
```

The file already includes `co_routine.h`, which now declares the function.

- [ ] **Step 8: Verify public API test passes**

Run:

```bash
make -C test test_public_api
./test/test_public_api
```

Expected after implementation: build succeeds and the test exits 0.

- [ ] **Step 9: Verify the full test runner still passes**

Run:

```bash
make check
```

Expected after implementation: all four test binaries run with exit code 0.

- [ ] **Step 10: Commit public API compatibility changes**

```bash
git add co_routine.h co_routine.cpp example/example_echosvr.cpp test/test_public_api.cpp test/Makefile CMakeLists.txt
git commit -m "fix: make public coroutine APIs linkable"
```

## Task 3: README And Example Documentation

**Files:**
- Modify: `README.md`
- Create: `example/README.md`
- Modify: `example/example_echocli.cpp`

- [ ] **Step 1: Replace README with current repository documentation**

Replace `README.md` with this content:

````markdown
# Libco

Libco is a small C++17 coroutine library inspired by Tencent's original libco.
This repository provides stackful coroutines, coroutine-aware polling and socket
hooks, a simple condition primitive, and a small Future/Promise async layer.

The current tree keeps the legacy `co_*` API names for compatibility and also
exposes C++ classes such as `co::Coroutine`, `co::CoCond`, `co::Future`,
`co::Promise`, and `co::ThreadWorker`.

## Public API Groups

| Area | Main headers | Main names |
| --- | --- | --- |
| Coroutine lifecycle | `co_routine.h` | `co_create`, `co_resume`, `co_yield_ct`, `co_free`, `co::Coroutine` |
| Event loop and polling | `co_routine.h` | `co_poll`, `co_eventloop`, hooked `poll` |
| Socket hooks | `co_routine.h` | `co_enable_hook_sys`, `co_disable_hook_sys`, `co_accept` |
| Conditions | `co_cond.h` | `co::CoCond::Signal`, `Broadcast`, `Timedwait` |
| Async tasks | `co_async.h`, `co_future.h` | `co::async`, `co::Future`, `co::Promise` |
| Worker loop | `thread_worker.h` | `co::ThreadWorker::run_loop` |

## Build

Using Make:

```bash
make all
```

Using CMake:

```bash
cmake -S . -B build
cmake --build build
```

## Test

Using Make:

```bash
make check
```

Using CMake:

```bash
cmake -S . -B build
cmake --build build
cd build && ctest --output-on-failure
```

## Examples

Examples live in `example/`. See `example/README.md` for each binary, the concept
it demonstrates, and a minimal command.

## Feature Status

| Feature | Status in this repository |
| --- | --- |
| Stackful coroutine create/resume/yield | Implemented and tested |
| Coroutine-aware `poll` and socket hooks | Implemented and covered by examples/tests |
| Duplicate fd behavior in hooked `poll` | Tested by `test/test_co_poll.cpp` |
| Coroutine condition primitive | Implemented and demonstrated by `example_cond` |
| Future/Promise async helper | Implemented and tested by `test/test_co_async.cpp` |
| Echo server/client examples | Implemented in `example_echosvr` and `example_echocli` |
| CGI, mysqlclient, ssl, gethostbyname adapters | Not provided in this repository |
| Shared-stack/copy-stack examples | Not provided in this repository |

## Compatibility Notes

The legacy `co_*` functions remain supported. New code can use the C++ class
interfaces directly where they are clearer, but existing code does not need to
change include paths or public API names.
````

- [ ] **Step 2: Add example navigation**

Create `example/README.md`:

````markdown
# Libco Examples

Build examples from the repository root:

```bash
make examples
```

| Binary | Concept | Arguments | Minimal command |
| --- | --- | --- | --- |
| `example_cond` | Producer/consumer coordination with `CoCond` | none | `./example/example_cond` |
| `example_echosvr` | Coroutine echo server with worker threads | `ip port workers` | `./example/example_echosvr 0.0.0.0 8080 4` |
| `example_echocli` | Coroutine echo client | `ip port connections loops` | `./example/example_echocli 127.0.0.1 8080 10 100` |
| `example_poll` | Coroutine-aware `poll` over many sockets | `ip port coroutine_count fd_count` | `./example/example_poll 127.0.0.1 8080 10 100` |
| `example_setenv` | Coroutine-local environment hook behavior | none | `./example/example_setenv` |
| `example_thread` | Event loops in multiple pthreads | `thread_count` | `./example/example_thread 4` |

For the client and poll examples, start a compatible server first, such as
`example_echosvr`.
````

- [ ] **Step 3: Fix echo client trailing sample comment**

In `example/example_echocli.cpp`, replace the trailing command comment that names the server binary as the client command. The corrected trailing comment should be:

```cpp
// ./example_echocli 127.0.0.1 1024 100 100
```

- [ ] **Step 4: Verify markdown references and example names**

Run:

```bash
rg "example_(closure|copystack|specific|exit)" README.md example/README.md CMakeLists.txt example/Makefile
rg "example_echosvr 127\\.0\\.0\\.1" example/example_echocli.cpp
```

Expected after implementation: both `rg` commands print no output and exit 1 because there are no matches.

- [ ] **Step 5: Verify build and tests still pass**

Run:

```bash
make check
```

Expected after implementation: all test binaries pass.

- [ ] **Step 6: Commit documentation changes**

```bash
git add README.md example/README.md example/example_echocli.cpp
git commit -m "docs: describe current libco layout"
```

## Task 4: Small Local Refactors

**Files:**
- Modify: `co_cond.cpp`
- Modify: `thread_worker.cpp`
- Modify: `coctx.cpp`

- [ ] **Step 1: Run current tests before small refactors**

Run:

```bash
make check
```

Expected before implementation: all current tests pass.

- [ ] **Step 2: Extract condition waiter activation**

In `co_cond.cpp`, add this helper after `OnSignalProcessEvent`:

```cpp
static void ActivateWaiter(CoCondItem *cond_item) {
  TimeoutItemLink::remove(&cond_item->timeout);
  co_get_curr_thread_env()->Epoll()->active_list()->add_tail(
      &cond_item->timeout);
}
```

Replace `Signal()` and `Broadcast()` with:

```cpp
int CoCond::Signal() {
  CoCondItem *cond_item = Pop();
  if (!cond_item) {
    return 0;
  }
  ActivateWaiter(cond_item);
  return 0;
}

int CoCond::Broadcast() {
  for (;;) {
    CoCondItem *cond_item = Pop();
    if (!cond_item) {
      return 0;
    }
    ActivateWaiter(cond_item);
  }
}
```

- [ ] **Step 3: Clarify task coroutine ownership**

In `thread_worker.cpp`, add this helper above `spawn_pending_tasks()`:

```cpp
static void run_task_and_mark_finished(std::unique_ptr<Task> task) {
    task->run();
    active_coroutine_count--;
    finished_coroutines.push_back(co_self());
}

static Coroutine* create_task_coroutine(std::unique_ptr<Task> task) {
    Task* task_ptr = task.release();
    return co_create([task_ptr]() {
        std::unique_ptr<Task> owned_task(task_ptr);
        run_task_and_mark_finished(std::move(owned_task));
    });
}
```

Replace `spawn_pending_tasks()` with:

```cpp
static void spawn_pending_tasks() {
    while (!pending_tasks.empty()) {
        std::unique_ptr<Task> task = std::move(pending_tasks.front());
        pending_tasks.pop_front();
        Coroutine* coroutine = create_task_coroutine(std::move(task));
        active_coroutine_count++;
        co_resume(coroutine);
    }
}
```

- [ ] **Step 4: Move duplicated context initialization**

In `coctx.cpp`, keep a single `coctx_init()` before the architecture-specific `coctx_make()` branches:

```cpp
int coctx_init(coctx_t *ctx) {
  memset(ctx, 0, sizeof(*ctx));
  return 0;
}
```

Delete the duplicate `coctx_init()` definitions inside the `__i386__` and `__x86_64__` branches. Leave both `coctx_make()` implementations otherwise unchanged.

- [ ] **Step 5: Verify small refactors**

Run:

```bash
make check
```

Expected after implementation: all test binaries pass.

- [ ] **Step 6: Commit small local refactors**

```bash
git add co_cond.cpp thread_worker.cpp coctx.cpp
git commit -m "refactor: clarify small runtime helpers"
```

## Task 5: `co_routine.cpp` Poll And Event Loop Cleanup

**Files:**
- Modify: `co_routine.cpp`

- [ ] **Step 1: Run focused poll regression before refactor**

Run:

```bash
make -C test test_co_poll
./test/test_co_poll
```

Expected before implementation: build succeeds and `test_co_poll` exits 0.

- [ ] **Step 2: Add a poll state owner**

In `co_routine.cpp`, after `struct PollItem`, add this state owner:

```cpp
static constexpr nfds_t kStackPollItemCount = 2;

class PollState {
public:
  PollState(EpollCtx *ep_ctx, nfds_t nfds, Coroutine *owner)
      : poll_(std::make_unique<PollBase>()) {
    poll_->epoll_fd = ep_ctx->fd();
    poll_->fds = new pollfd[nfds];
    poll_->nfds = nfds;
    poll_->poll_items = nfds < kStackPollItemCount
                            ? stack_items_
                            : new PollItem[nfds];
    poll_->process_func = PollProcessFunc;
    poll_->arg = owner;
  }

  ~PollState() {
    if (poll_->poll_items != stack_items_) {
      delete[] poll_->poll_items;
      poll_->poll_items = nullptr;
    }
    delete[] poll_->fds;
    poll_->fds = nullptr;
  }

  PollBase *poll() { return poll_.get(); }

private:
  std::unique_ptr<PollBase> poll_;
  PollItem stack_items_[kStackPollItemCount];
};
```

This removes manual `new PollBase()` and `delete (&arg)` ownership without changing the lifetime of the timeout item while the coroutine is suspended.

- [ ] **Step 3: Extract fd registration**

Move the existing `poll_func_t` typedef so it appears before the new helper:

```cpp
typedef int (*poll_func_t)(struct pollfd fds[], nfds_t nfds, int timeout);
```

Then add this helper after the typedef:

```cpp
static bool RegisterPollFds(EpollCtx *ep_ctx, PollBase *poll,
                            struct pollfd fds[], nfds_t nfds,
                            poll_func_t poll_func, int timeout) {
  for (nfds_t i = 0; i < nfds; i++) {
    poll->poll_items[i].self_pfd = poll->fds + i;
    poll->poll_items[i].poll = poll;
    poll->poll_items[i].prepare_func = PollPrepareFunc;
    struct epoll_event &ev = poll->poll_items[i].ep_event;

    if (fds[i].fd > -1) {
      ev.data.ptr = poll->poll_items + i;
      ev.events = PollEvent2Epoll(fds[i].events);

      int ret = ep_ctx->add(fds[i].fd, &ev);
      if (ret < 0 && errno == EPERM && nfds == 1 && poll_func != nullptr) {
        return false;
      }
    }
  }
  return true;
}
```

- [ ] **Step 4: Extract poll cleanup and copyback**

Add this helper after `RegisterPollFds`:

```cpp
static void CleanupPoll(EpollCtx *ep_ctx, PollBase *poll,
                        struct pollfd fds[], nfds_t nfds) {
  TimeoutItemLink::remove(poll);
  for (nfds_t i = 0; i < nfds; i++) {
    int fd = fds[i].fd;
    if (fd > -1) {
      ep_ctx->del(fd, &poll->poll_items[i].ep_event);
    }
    fds[i].revents = poll->fds[i].revents;
  }
}
```

- [ ] **Step 5: Rewrite `co_poll_inner()` around the helpers**

Replace `co_poll_inner()` with:

```cpp
int co_poll_inner(struct pollfd fds[], nfds_t nfds, int timeout,
                  poll_func_t poll_func) {
  EpollCtx *ep_ctx = co_get_epoll_ct();
  if (timeout == 0 && poll_func != nullptr) {
    return poll_func(fds, nfds, timeout);
  }
  if (timeout < 0) {
    timeout = INT_MAX;
  }

  PollState state(ep_ctx, nfds, co_self());
  PollBase *poll = state.poll();

  if (!RegisterPollFds(ep_ctx, poll, fds, nfds, poll_func, timeout)) {
    return poll_func(fds, nfds, timeout);
  }

  unsigned long long now = GetTickMS();
  poll->expire_time_ms = now + timeout;
  int ret = ep_ctx->timeout()->AddItem(poll, now);
  int raise_cnt = 0;
  if (ret != 0) {
    co_log_err(
        "CO_ERR: AddItem ret %d now %lld timeout %d arg.expire_time_ms %lld",
        ret, now, timeout, poll->expire_time_ms);
    errno = EINVAL;
    raise_cnt = -1;
  } else {
    co_yield_ct();
    raise_cnt = poll->raise_cnt;
  }

  CleanupPoll(ep_ctx, poll, fds, nfds);
  return raise_cnt;
}
```

- [ ] **Step 6: Extract event loop helpers**

Add these helpers above `co_eventloop()`:

```cpp
static void CollectReadyEvents(EpollCtx *ep_ctx, int event_count,
                               TimeoutItemLink *active) {
  for (int i = 0; i < event_count; i++) {
    epoll_event &ev = ep_ctx->events()->events[i];
    TimeoutItem *item = (TimeoutItem *)ev.data.ptr;
    if (item->prepare_func) {
      item->prepare_func(item, ev, active);
    } else {
      active->add_tail(item);
    }
  }
}

static unsigned long long CollectTimeouts(EpollCtx *ep_ctx,
                                          TimeoutItemLink *timeout) {
  unsigned long long now = GetTickMS();
  ep_ctx->timeout()->TakeAll(now, timeout);

  TimeoutItem *item = timeout->head;
  while (item) {
    item->timeout = true;
    item = item->next;
  }
  return now;
}

static void DispatchActiveItems(EpollCtx *ep_ctx, TimeoutItemLink *active,
                                TimeoutItemLink *timeout,
                                unsigned long long now) {
  active->join(*timeout);

  TimeoutItem *item = active->head;
  while (item) {
    active->pop_head();
    if (item->timeout && now < item->expire_time_ms) {
      int ret = ep_ctx->timeout()->AddItem(item, now);
      if (!ret) {
        item->timeout = false;
        item = active->head;
        continue;
      }
    }
    if (item->process_func) {
      item->process_func(item);
    }

    item = active->head;
  }
}
```

- [ ] **Step 7: Rewrite `co_eventloop()` around the helpers**

Replace the body of `co_eventloop()` with:

```cpp
void co_eventloop(pfn_co_eventloop_t func, void *arg) {
  EpollCtx *ep_ctx = co_get_epoll_ct();

  for (;;) {
    int ret = ep_ctx->wait(1);
    TimeoutItemLink *active = ep_ctx->active_list();
    TimeoutItemLink *timeout = ep_ctx->timeout_list();
    timeout->clear();

    CollectReadyEvents(ep_ctx, ret, active);
    unsigned long long now = CollectTimeouts(ep_ctx, timeout);
    DispatchActiveItems(ep_ctx, active, timeout, now);

    if (func && -1 == func(arg)) {
      break;
    }
  }
}
```

- [ ] **Step 8: Verify poll and full tests**

Run:

```bash
make -C test test_co_poll
./test/test_co_poll
make check
```

Expected after implementation: `test_co_poll` and the full test runner pass.

- [ ] **Step 9: Commit `co_routine.cpp` cleanup**

```bash
git add co_routine.cpp
git commit -m "refactor: clarify coroutine poll loop"
```

## Task 6: `co_hook_sys_call.cpp` Hook Helper Cleanup

**Files:**
- Modify: `co_hook_sys_call.cpp`

- [ ] **Step 1: Run current tests before hook refactor**

Run:

```bash
make check
```

Expected before implementation: all test binaries pass.

- [ ] **Step 2: Add shared hook helpers**

In `co_hook_sys_call.cpp`, after `free_by_fd`, add:

```cpp
static inline int timeval_to_ms(const struct timeval &timeout) {
  return (timeout.tv_sec * 1000) + (timeout.tv_usec / 1000);
}

static inline bool should_bypass_hook(int fd, rpchook_t **hook) {
  if (!co_is_enable_sys_hook()) {
    *hook = nullptr;
    return true;
  }

  *hook = get_by_fd(fd);
  return !*hook || ((*hook)->user_flag & O_NONBLOCK);
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
  ssize_t write_ret = write_once((const char *)buffer + written,
                                 length - written);

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
```

- [ ] **Step 3: Use helpers in read-like wrappers**

Replace timeout and poll setup in `read()` with:

```cpp
  rpchook_t *lp = nullptr;
  if (should_bypass_hook(fd, &lp)) {
    return g_sys_read_func(fd, buf, nbyte);
  }
  int timeout = timeval_to_ms(lp->read_timeout);

  int pollret = wait_for_fd(fd, POLLIN | POLLERR | POLLHUP, timeout);

  ssize_t readret = g_sys_read_func(fd, (char *)buf, nbyte);
```

Replace timeout and poll setup in `recv()` with:

```cpp
  rpchook_t *lp = nullptr;
  if (should_bypass_hook(socket, &lp)) {
    return g_sys_recv_func(socket, buffer, length, flags);
  }
  int timeout = timeval_to_ms(lp->read_timeout);

  int pollret = wait_for_fd(socket, POLLIN | POLLERR | POLLHUP, timeout);

  ssize_t readret = g_sys_recv_func(socket, buffer, length, flags);
```

Replace timeout and poll setup in `recvfrom()` with:

```cpp
  rpchook_t *lp = nullptr;
  if (should_bypass_hook(socket, &lp)) {
    return g_sys_recvfrom_func(socket, buffer, length, flags, address,
                               address_len);
  }

  int timeout = timeval_to_ms(lp->read_timeout);
  wait_for_fd(socket, POLLIN | POLLERR | POLLHUP, timeout);

  ssize_t ret =
      g_sys_recvfrom_func(socket, buffer, length, flags, address, address_len);
```

- [ ] **Step 4: Use helpers in write-like wrappers**

Replace `write()` body after the hook function lookup with:

```cpp
  rpchook_t *lp = nullptr;
  if (should_bypass_hook(fd, &lp)) {
    return g_sys_write_func(fd, buf, nbyte);
  }

  int timeout = timeval_to_ms(lp->write_timeout);
  return write_with_retry(fd, buf, nbyte, timeout,
                          [fd](const char *data, size_t len) {
                            return g_sys_write_func(fd, data, len);
                          });
```

Replace `send()` body after the hook function lookup with:

```cpp
  rpchook_t *lp = nullptr;
  if (should_bypass_hook(socket, &lp)) {
    return g_sys_send_func(socket, buffer, length, flags);
  }

  int timeout = timeval_to_ms(lp->write_timeout);
  return write_with_retry(socket, buffer, length, timeout,
                          [socket, flags](const char *data, size_t len) {
                            return g_sys_send_func(socket, data, len, flags);
                          });
```

Replace `sendto()` timeout wait block with:

```cpp
  rpchook_t *lp = nullptr;
  if (should_bypass_hook(socket, &lp)) {
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
```

- [ ] **Step 5: Extract coroutine-local env lookup**

After `static stCoSysEnvArr_t g_co_sysenv = {0};`, add:

```cpp
static stCoSysEnv_t *find_coroutine_env(const char *name, bool create) {
  if (!co_is_enable_sys_hook() || !g_co_sysenv.data) {
    return nullptr;
  }

  Coroutine *self = co_self();
  if (!self) {
    return nullptr;
  }

  if (create && !self->GetSysEnvs()) {
    self->GetSysEnvs() = dup_co_sysenv_arr(&g_co_sysenv);
  }

  if (!self->GetSysEnvs()) {
    return nullptr;
  }

  stCoSysEnvArr_t *arr = (stCoSysEnvArr_t *)(self->GetSysEnvs());
  stCoSysEnv_t key = {(char *)name, 0};
  return (stCoSysEnv_t *)bsearch(&key, arr->data, arr->cnt, sizeof(key),
                                 co_sysenv_comp);
}
```

Use it in `setenv()`:

```cpp
  if (stCoSysEnv_t *e = find_coroutine_env(n, true)) {
    if (overwrite || !e->value) {
      if (e->value) {
        free(e->value);
      }
      assert(value != nullptr);
      e->value = strdup(value);
    }
    return 0;
  }
  return g_sys_setenv_func(n, value, overwrite);
```

Use it in `unsetenv()`:

```cpp
  if (stCoSysEnv_t *e = find_coroutine_env(n, true)) {
    if (e->value) {
      free(e->value);
      e->value = nullptr;
    }
    return 0;
  }
  return g_sys_unsetenv_func(n);
```

Use it in `getenv()`:

```cpp
  if (stCoSysEnv_t *e = find_coroutine_env(n, true)) {
    return e->value;
  }
  return g_sys_getenv_func(n);
```

- [ ] **Step 6: Make `fcntl()` hook cases auditable**

Add these helpers before `fcntl()`:

```cpp
static int handle_f_getfl(int fd, rpchook_t *hook) {
  int ret = g_sys_fcntl_func(fd, F_GETFL);
  if (hook && !(hook->user_flag & O_NONBLOCK)) {
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
```

In the `fcntl()` switch, replace only `F_GETFL` and `F_SETFL` cases:

```cpp
  case F_GETFL: {
    ret = handle_f_getfl(fildes, lp);
    break;
  }
  case F_SETFL: {
    int param = va_arg(arg_list, int);
    ret = handle_f_setfl(fildes, param, lp);
    break;
  }
```

Leave other command cases behaviorally unchanged.

- [ ] **Step 7: Verify hook refactor**

Run:

```bash
make check
```

Expected after implementation: all test binaries pass.

- [ ] **Step 8: Commit hook helper cleanup**

```bash
git add co_hook_sys_call.cpp
git commit -m "refactor: clarify syscall hook helpers"
```

## Task 7: Final Verification And Cleanup

**Files:**
- Verify all modified files

- [ ] **Step 1: Check worktree**

Run:

```bash
git status --short
```

Expected: no unexpected untracked or modified files except intentional files from the current task.

- [ ] **Step 2: Run Makefile build**

Run:

```bash
make all
```

Expected: library, examples, and tests build successfully.

- [ ] **Step 3: Run Makefile tests**

Run:

```bash
make check
```

Expected: all test binaries pass, including `test_co_poll` and `test_public_api`.

- [ ] **Step 4: Run CMake verification**

Run:

```bash
cmake -S . -B /tmp/libco-cmake-final
cmake --build /tmp/libco-cmake-final
cd /tmp/libco-cmake-final && ctest --output-on-failure
```

Expected: configure, build, and CTest all succeed.

- [ ] **Step 5: Check stale references**

Run:

```bash
rg "example_(closure|copystack|specific|exit)" README.md example/README.md CMakeLists.txt example/Makefile
rg "int co_accept\\(" example/example_echosvr.cpp
```

Expected: both `rg` commands print no output and exit 1 because there are no matches.

- [ ] **Step 6: Review commit history**

Run:

```bash
git log --oneline -8
```

Expected: task commits are present in order after the design/spec commits.

- [ ] **Step 7: Final status**

Run:

```bash
git status --short
```

Expected: clean worktree.
