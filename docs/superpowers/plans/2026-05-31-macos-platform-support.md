# macOS Platform Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the current libco tree compile and pass its basic tests on native Apple Silicon macOS.

**Architecture:** Keep the existing Linux path intact and add only the platform branching needed for arm64 Darwin. CMake is the primary path; Make uses the same platform decisions so `make check` remains available. macOS arm64 uses the existing `USE_UCONTEXT` backend, while Linux/x86 continues using `coctx_swap`.

**Tech Stack:** C++17, AppleClang, CMake, GNU Make/BSD make-compatible makefiles, POSIX `ucontext`, kqueue-backed epoll compatibility.

---

## File Structure

- Modify `CMakeLists.txt`: modernize the minimum CMake version, select arm64 macOS compile options, omit x86 context-switch sources when `USE_UCONTEXT` is active, and keep CMake tests registered.
- Modify `co_timeout.h`: stop including Linux-only `<sys/epoll.h>` directly and use the existing compatibility header.
- Modify `routine_context.h`: include `<ucontext.h>` only when the ucontext backend is selected.
- Modify `co_epoll.cpp`: correct macOS kqueue timeout conversion and preserve error returns.
- Modify `co.mk`: centralize platform flags, platform libraries, and the `USE_UCONTEXT` make variable.
- Modify `Makefile`: use platform libraries and conditionally include x86 context-switch objects.
- Modify `example/Makefile`: use platform libraries and remove hard-coded Linux/macOS-incompatible flags.
- Modify `test/Makefile`: use platform libraries and remove hard-coded Linux/macOS-incompatible flags.
- Modify `README.md`: document the Apple Silicon macOS support target and verified commands.

## Task 1: CMake and Source Platform Fixes

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `co_timeout.h`
- Modify: `routine_context.h`
- Modify: `co_epoll.cpp`

- [ ] **Step 1: Reproduce the current CMake failures**

Run:

```bash
cmake -S . -B /private/tmp/libco-cmake-baseline
```

Expected: the command exits nonzero and prints a message containing:

```text
Compatibility with CMake < 3.5 has been removed from CMake.
```

Then run:

```bash
cmake -S . -B /private/tmp/libco-cmake-baseline -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build /private/tmp/libco-cmake-baseline -- -j2
```

Expected: configure succeeds, build exits nonzero, and the compiler output contains:

```text
co_timeout.h:5:10: fatal error: 'sys/epoll.h' file not found
```

- [ ] **Step 2: Replace `CMakeLists.txt` with a platform-aware version**

Replace the complete contents of `CMakeLists.txt` with:

```cmake
cmake_minimum_required(VERSION 3.10)
project(libco LANGUAGES C CXX ASM)

set(CMAKE_MACOSX_RPATH 0)
set(LIBCO_VERSION 0.5)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(LIBCO_COMMON_COMPILE_OPTIONS
        -g
        -fno-strict-aliasing
        -O2
        -Wall
        -Werror
        -pipe
        -D_REENTRANT
        -fPIC
        -Wno-deprecated)

set(LIBCO_PLATFORM_LIBS pthread)
set(LIBCO_USE_UCONTEXT OFF)

if(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
        set(LIBCO_USE_UCONTEXT ON)
        list(APPEND LIBCO_COMMON_COMPILE_OPTIONS
                -D_XOPEN_SOURCE
                -DUSE_UCONTEXT
                -Wno-deprecated-declarations)
    endif()
else()
    list(APPEND LIBCO_COMMON_COMPILE_OPTIONS
            -D_GNU_SOURCE
            -m64)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -export-dynamic")
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
        list(APPEND LIBCO_PLATFORM_LIBS dl)
    endif()
endif()

set(LIBCO_SOURCE_FILES
        co_epoll.cpp
        co_cond.cpp
        thread_worker.cpp
        routine_context.cpp
        co_routine.cpp
        co_hook_sys_call.cpp)

if(NOT LIBCO_USE_UCONTEXT)
    list(APPEND LIBCO_SOURCE_FILES
            coctx.cpp
            coctx_swap.S)
endif()

add_library(colib_static STATIC ${LIBCO_SOURCE_FILES})
add_library(colib_shared SHARED ${LIBCO_SOURCE_FILES})

foreach(LIBCO_TARGET colib_static colib_shared)
    target_include_directories(${LIBCO_TARGET} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${LIBCO_TARGET} PRIVATE ${LIBCO_COMMON_COMPILE_OPTIONS})
    set_target_properties(${LIBCO_TARGET} PROPERTIES
            OUTPUT_NAME colib
            CLEAN_DIRECT_OUTPUT 1)
endforeach()

set_target_properties(colib_shared PROPERTIES
        VERSION ${LIBCO_VERSION}
        SOVERSION ${LIBCO_VERSION})

macro(add_example_target EXAMPLE_TARGET)
    add_executable("${EXAMPLE_TARGET}" "example/${EXAMPLE_TARGET}.cpp")
    target_compile_options("${EXAMPLE_TARGET}" PRIVATE ${LIBCO_COMMON_COMPILE_OPTIONS})
    target_link_libraries("${EXAMPLE_TARGET}" colib_static ${LIBCO_PLATFORM_LIBS})
endmacro()

add_example_target(example_cond)
add_example_target(example_echocli)
add_example_target(example_echosvr)
add_example_target(example_poll)
add_example_target(example_setenv)
add_example_target(example_thread)

enable_testing()

macro(add_libco_test TEST_TARGET)
    add_executable("${TEST_TARGET}" "test/${TEST_TARGET}.cpp")
    target_compile_options("${TEST_TARGET}" PRIVATE ${LIBCO_COMMON_COMPILE_OPTIONS})
    target_link_libraries("${TEST_TARGET}" colib_static ${LIBCO_PLATFORM_LIBS})
    add_test(NAME "${TEST_TARGET}" COMMAND "${TEST_TARGET}")
endmacro()

add_libco_test(test_co_routine)
add_libco_test(test_co_async)
add_libco_test(test_co_poll)
add_libco_test(test_public_api)
```

- [ ] **Step 3: Route timeout code through the epoll compatibility header**

In `co_timeout.h`, replace:

```cpp
#include "co_link.h"
#include "util.h"
#include <sys/epoll.h>
```

with:

```cpp
#include "co_epoll.h"
#include "co_link.h"
#include "util.h"
```

- [ ] **Step 4: Include ucontext only for the ucontext backend**

In `routine_context.h`, replace:

```cpp
#pragma once
#include "coctx.h"
#include <ucontext.h>
```

with:

```cpp
#pragma once
#include "coctx.h"

#ifdef USE_UCONTEXT
#include <ucontext.h>
#endif
```

- [ ] **Step 5: Correct macOS kqueue timeout and error handling**

In `co_epoll.cpp`, inside the macOS/BSD `#else` block, replace the current `co_epoll_wait` function:

```cpp
static int co_epoll_wait(int epfd, struct co_epoll_res *events, int maxevents,
                         int timeout) {
  struct timespec t = {0};
  if (timeout > 0) {
    t.tv_sec = timeout;
  }
  int ret = kevent(epfd, nullptr, 0,             // register null
                   events->eventlist, maxevents, // just retrival
                   (-1 == timeout) ? nullptr : &t);
  int j = 0;
  for (int i = 0; i < ret; i++) {
    struct kevent &kev = events->eventlist[i];
    struct kevent_pair_t *ptr = (struct kevent_pair_t *)kev.udata;
    struct epoll_event *ev = events->events + i;
    if (0 == ptr->fire_idx) {
      ptr->fire_idx = i + 1;
      memset(ev, 0, sizeof(*ev));
      ++j;
    } else {
      ev = events->events + ptr->fire_idx - 1;
    }
    if (EVFILT_READ == kev.filter) {
      ev->events |= EPOLLIN;
    } else if (EVFILT_WRITE == kev.filter) {
      ev->events |= EPOLLOUT;
    }
    ev->data.u64 = ptr->u64;
  }
  for (int i = 0; i < ret; i++) {
    ((struct kevent_pair_t *)(events->eventlist[i].udata))->fire_idx = 0;
  }
  return j;
}
```

with:

```cpp
static struct timespec milliseconds_to_timespec(int timeout_ms) {
  struct timespec t = {0};
  if (timeout_ms > 0) {
    t.tv_sec = timeout_ms / 1000;
    t.tv_nsec = (timeout_ms % 1000) * 1000000;
  }
  return t;
}

static int co_epoll_wait(int epfd, struct co_epoll_res *events, int maxevents,
                         int timeout) {
  struct timespec t = milliseconds_to_timespec(timeout);
  int ret = kevent(epfd, nullptr, 0,              // register null
                   events->eventlist, maxevents, // just retrival
                   (-1 == timeout) ? nullptr : &t);
  if (ret <= 0) {
    return ret;
  }

  int j = 0;
  for (int i = 0; i < ret; i++) {
    struct kevent &kev = events->eventlist[i];
    struct kevent_pair_t *ptr = (struct kevent_pair_t *)kev.udata;
    if (!ptr) {
      errno = EINVAL;
      return -1;
    }

    struct epoll_event *ev = events->events + i;
    if (0 == ptr->fire_idx) {
      ptr->fire_idx = i + 1;
      memset(ev, 0, sizeof(*ev));
      ++j;
    } else {
      ev = events->events + ptr->fire_idx - 1;
    }
    if (EVFILT_READ == kev.filter) {
      ev->events |= EPOLLIN;
    } else if (EVFILT_WRITE == kev.filter) {
      ev->events |= EPOLLOUT;
    }
    ev->data.u64 = ptr->u64;
  }
  for (int i = 0; i < ret; i++) {
    ((struct kevent_pair_t *)(events->eventlist[i].udata))->fire_idx = 0;
  }
  return j;
}
```

In the same file, replace:

```cpp
  if (ev->events & ~flags) {
    return -1;
  }
```

with:

```cpp
  if (ev->events & ~flags) {
    errno = EINVAL;
    return -1;
  }
```

- [ ] **Step 6: Verify the CMake path**

Run:

```bash
cmake -S . -B /private/tmp/libco-cmake-macos
cmake --build /private/tmp/libco-cmake-macos -- -j2
cd /private/tmp/libco-cmake-macos && ctest --output-on-failure
```

Expected: configure exits 0, build exits 0, and ctest reports all four tests passed:

```text
100% tests passed
```

- [ ] **Step 7: Commit the CMake and source platform fix**

Run:

```bash
git add CMakeLists.txt co_timeout.h routine_context.h co_epoll.cpp
git commit -m "build: support cmake on apple silicon macos"
```

Expected: commit exits 0 and records the CMake/source platform changes.

## Task 2: Make Build Path Support

**Files:**
- Modify: `co.mk`
- Modify: `Makefile`
- Modify: `example/Makefile`
- Modify: `test/Makefile`

- [ ] **Step 1: Reproduce the current Make failure after Task 1**

Run:

```bash
make clean
make check
```

Expected before this task's changes: `make check` exits nonzero on Apple Silicon macOS. The output should include one of these macOS-incompatible build assumptions:

```text
unsupported option '-m64'
```

or:

```text
library not found for -ldl
```

- [ ] **Step 2: Replace `co.mk` with platform-aware shared make rules**

Replace the complete contents of `co.mk` with:

```make
#
# Tencent is pleased to support the open source community by making Libco available.
#
# Copyright (C) 2014 THL A29 Limited, a Tencent company. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License"); 
# you may not use this file except in compliance with the License. 
# You may obtain a copy of the License at
#
#	http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, 
# software distributed under the License is distributed on an "AS IS" BASIS, 
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
# See the License for the specific language governing permissions and 
# limitations under the License.
#

##### Makefile Rules ##########
MAIL_ROOT=.
SRCROOT=.

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

USE_UCONTEXT ?= 0
PLATFORM_CFLAGS :=
PLATFORM_LDFLAGS :=
PLATFORM_LIBS := -lpthread
PLATFORM_SHARED_FLAG := -shared

ifeq ($(UNAME_S),Darwin)
  PLATFORM_SHARED_FLAG := -dynamiclib
  ifeq ($(UNAME_M),arm64)
    USE_UCONTEXT := 1
    PLATFORM_CFLAGS += -D_XOPEN_SOURCE -DUSE_UCONTEXT -Wno-deprecated-declarations
  endif
else
  PLATFORM_CFLAGS += -D_GNU_SOURCE -DLINUX -m64
  PLATFORM_LDFLAGS += -export-dynamic
  ifneq ($(UNAME_S),FreeBSD)
    PLATFORM_LIBS += -ldl
  endif
endif

##define the compliers
CPP = $(CXX)
AR = ar -rc
RANLIB = ranlib

CPPSHARE = $(CPP) -fPIC --std=c++17 $(PLATFORM_SHARED_FLAG) -O2 -Wall -Werror -pipe $(PLATFORM_CFLAGS) -L$(SRCROOT)/solib/ -o 
CSHARE = $(CC) -fPIC --std=c++17 $(PLATFORM_SHARED_FLAG) -O2 -Wall -Werror -pipe $(PLATFORM_CFLAGS) -L$(SRCROOT)/solib/ -o 

ifeq ($v,release)
CFLAGS= $(INCLS) -fPIC --std=c++17 -O2 -Wall -Werror $(PLATFORM_CFLAGS) -pipe -Wno-deprecated -c
else
CFLAGS= -g $(INCLS) -fPIC --std=c++17 -O0 -Wall -Werror $(PLATFORM_CFLAGS) -pipe -c -fno-inline
endif

ifneq ($v,release)
BFLAGS= -g
endif

STATICLIBPATH=$(SRCROOT)/lib
DYNAMICLIBPATH=$(SRCROOT)/solib

INCLS += -I$(SRCROOT)

## default links
ifeq ($(LINKS_DYNAMIC), 1)
LINKS += -L$(DYNAMICLIBPATH) -L$(STATICLIBPATH)
else
LINKS += -L$(STATICLIBPATH)
endif

CPPSRCS  = $(wildcard *.cpp)
CSRCS  = $(wildcard *.c)
CPPOBJS  = $(patsubst %.cpp,%.o,$(CPPSRCS))
COBJS  = $(patsubst %.c,%.o,$(CSRCS))

SRCS = $(CPPSRCS) $(CSRCS)
OBJS = $(CPPOBJS) $(COBJS)

CPPCOMPI=$(CPP) $(CFLAGS) -Wno-deprecated
CCCOMPI=$(CC) $(CFLAGS)

BUILDEXE = $(CPP) $(BFLAGS) $(PLATFORM_LDFLAGS) -o $@ $^ $(LINKS) 
CLEAN = rm -f *.o 

CPPCOMPILE = $(CPPCOMPI) $< $(FLAGS) $(INCLS) $(MTOOL_INCL) -o $@
CCCOMPILE = $(CCCOMPI) $< $(FLAGS) $(INCLS) $(MTOOL_INCL) -o $@

ARSTATICLIB = $(AR) $@.tmp $^ $(AR_FLAGS); \
			  if [ $$? -ne 0 ]; then exit 1; fi; \
			  test -d $(STATICLIBPATH) || mkdir -p $(STATICLIBPATH); \
			  mv -f $@.tmp $(STATICLIBPATH)/$@;

BUILDSHARELIB = $(CPPSHARE) $@.tmp $^ $(BS_FLAGS); \
				if [ $$? -ne 0 ]; then exit 1; fi; \
				test -d $(DYNAMICLIBPATH) || mkdir -p $(DYNAMICLIBPATH); \
				mv -f $@.tmp $(DYNAMICLIBPATH)/$@;

.cpp.o:
	$(CPPCOMPILE)
.c.o:
	$(CCCOMPILE)
```

- [ ] **Step 3: Update the top-level Makefile flags, libraries, and object list**

In `Makefile`, replace the options/link/object block:

```make
########## options ##########
CFLAGS += -g -fno-strict-aliasing -O2 --std=c++17 -Wall -Werror -export-dynamic \
	-pipe -D_GNU_SOURCE -D_REENTRANT -fPIC -Wno-deprecated -m64

UNAME := $(shell uname -s)

ifeq ($(UNAME), FreeBSD)
LINKS += -g -L./lib -lcolib -lpthread
else
LINKS += -g -L./lib -lcolib -lpthread -ldl
endif

COLIB_OBJS=co_epoll.o co_cond.o thread_worker.o routine_context.o co_routine.o co_hook_sys_call.o coctx_swap.o coctx.o
```

with:

```make
########## options ##########
CFLAGS += -g -fno-strict-aliasing -O2 --std=c++17 -Wall -Werror \
	-pipe -D_REENTRANT -fPIC -Wno-deprecated

LINKS += -g -L./lib -lcolib $(PLATFORM_LIBS)

COLIB_OBJS=co_epoll.o co_cond.o thread_worker.o routine_context.o co_routine.o co_hook_sys_call.o

ifeq ($(USE_UCONTEXT),0)
COLIB_OBJS += coctx_swap.o coctx.o
endif
```

- [ ] **Step 4: Update `example/Makefile` flags and libraries**

In `example/Makefile`, replace:

```make
########## options ##########
CFLAGS += -I../ -g -fno-strict-aliasing -O2 --std=c++17 -Wall -Werror -export-dynamic \
	-pipe  -D_GNU_SOURCE -D_REENTRANT -fPIC -Wno-deprecated -m64

UNAME := $(shell uname -s)

ifeq ($(UNAME), FreeBSD)
LINKS += -g -L../lib -lcolib -lpthread
else
LINKS += -g -L../lib -lcolib -lpthread -ldl
endif
```

with:

```make
########## options ##########
CFLAGS += -I../ -g -fno-strict-aliasing -O2 --std=c++17 -Wall -Werror \
	-pipe -D_REENTRANT -fPIC -Wno-deprecated

LINKS += -g -L../lib -lcolib $(PLATFORM_LIBS)
```

- [ ] **Step 5: Update `test/Makefile` flags and libraries**

In `test/Makefile`, replace:

```make
########## options ##########
CFLAGS += -I../ -g -fno-strict-aliasing -O2 --std=c++17 -Wall -Werror -export-dynamic \
	-pipe -D_GNU_SOURCE -D_REENTRANT -fPIC -Wno-deprecated -m64

UNAME := $(shell uname -s)

ifeq ($(UNAME), FreeBSD)
LINKS += -g -L../lib -lcolib -lpthread
else
LINKS += -g -L../lib -lcolib -lpthread -ldl
endif
```

with:

```make
########## options ##########
CFLAGS += -I../ -g -fno-strict-aliasing -O2 --std=c++17 -Wall -Werror \
	-pipe -D_REENTRANT -fPIC -Wno-deprecated

LINKS += -g -L../lib -lcolib $(PLATFORM_LIBS)
```

- [ ] **Step 6: Verify the Make path**

Run:

```bash
make clean
make check
```

Expected: `make check` exits 0 and prints each basic test name:

```text
RUN test_co_routine
RUN test_co_async
RUN test_co_poll
RUN test_public_api
```

- [ ] **Step 7: Commit the Make platform fix**

Run:

```bash
git add co.mk Makefile example/Makefile test/Makefile
git commit -m "build: support make check on macos"
```

Expected: commit exits 0 and records the Make build changes.

## Task 3: README macOS Notes

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Add macOS support notes to the Build section**

In `README.md`, replace the current `## Build` section:

````markdown
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
````

with:

````markdown
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
````

- [ ] **Step 2: Add risk-suite boundary notes to the Test section**

In `README.md`, after the CMake test command block:

```markdown
cmake -S . -B build
cmake --build build
cd build && ctest --output-on-failure
```

add:

```markdown

On macOS, the supported test target is the basic suite above. The `risk-check`
and `risk-diagnose` targets remain Linux-oriented diagnostics in this phase.
```

- [ ] **Step 3: Verify README formatting**

Run:

```bash
sed -n '20,80p' README.md
```

Expected: output shows the Make and CMake build commands, the `### macOS`
subsection, and the macOS risk-suite boundary note.

- [ ] **Step 4: Commit the documentation update**

Run:

```bash
git add README.md
git commit -m "docs: document macos build support"
```

Expected: commit exits 0 and records the README changes.

## Task 4: Final Verification

**Files:**
- Verify: `CMakeLists.txt`
- Verify: `co_timeout.h`
- Verify: `routine_context.h`
- Verify: `co_epoll.cpp`
- Verify: `co.mk`
- Verify: `Makefile`
- Verify: `example/Makefile`
- Verify: `test/Makefile`
- Verify: `README.md`

- [ ] **Step 1: Verify the final CMake acceptance path**

Run:

```bash
cmake -S . -B /private/tmp/libco-final-cmake
cmake --build /private/tmp/libco-final-cmake -- -j2
cd /private/tmp/libco-final-cmake && ctest --output-on-failure
```

Expected: configure exits 0, build exits 0, and ctest prints:

```text
100% tests passed
```

- [ ] **Step 2: Verify the final Make acceptance path**

Run from the repository root:

```bash
make clean
make check
```

Expected: `make check` exits 0 and prints:

```text
RUN test_co_routine
RUN test_co_async
RUN test_co_poll
RUN test_public_api
```

- [ ] **Step 3: Confirm risk targets are not part of acceptance**

Run:

```bash
git grep -n "risk-check\\|risk-diagnose" README.md docs/superpowers/specs/2026-05-31-macos-platform-support-design.md
```

Expected: output states that `risk-check` and `risk-diagnose` are excluded from macOS acceptance or remain Linux-oriented diagnostics.

- [ ] **Step 4: Inspect final git status**

Run:

```bash
git status --short
```

Expected: no uncommitted source, build, or documentation changes remain after the task commits.
