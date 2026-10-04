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

## Process and Thread Lifecycle

The supported startup order is:

```text
Single-threaded process: fork, if needed
  -> Each process: create worker threads, if needed
    -> Each execution thread: initialize its own ThreadEnv
      -> Create and run coroutines
```

Complete all `fork()` calls before creating worker threads. Initialize coroutine
environments within the threads that will execute their coroutines; a
single-threaded application may use its main thread.

After any thread initializes a coroutine environment, `fork()` is unsupported
throughout the process. This includes initialization performed implicitly by
coroutine or scheduling APIs. Disabling hooks, finishing coroutines, or
terminating worker threads does not restore fork support.

Avoiding coroutine initialization alone is not sufficient to make a later
multi-threaded `fork()` safe: `close()` and `fcntl()` can acquire the global fd
metadata mutex even when hooks are disabled and no coroutine environment exists.
A child could inherit a locked mutex whose owning thread no longer exists.

Applications are responsible for following this startup order. Libco does not
detect violations or recover inherited runtime state after an unsupported fork.
Tests that fork must keep the parent single-threaded and free of coroutine
environments, then initialize any threads and coroutine environments in the child.

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

### macOS

Native Apple Silicon macOS builds are supported for the basic library,
examples, and tests. On arm64 Darwin, the build uses the existing `ucontext`
backend instead of the x86 assembly context switch, and the event loop uses the
existing kqueue-backed epoll compatibility layer.

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

On macOS, the supported test target is the basic suite above. The `risk-check`
and `risk-diagnose` targets remain Linux-oriented diagnostics in this phase.

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
