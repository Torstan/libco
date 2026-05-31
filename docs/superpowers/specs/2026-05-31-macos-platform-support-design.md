# macOS Platform Support Design

Date: 2026-05-31
Status: Approved for implementation planning

## Purpose

Make this libco tree compile and run its basic test suite on the current local
Apple Silicon macOS environment. The support target is the developer's native
machine: arm64 Darwin with AppleClang.

## Scope

The implementation covers the minimum platform work needed for:

1. CMake configure, build, and `ctest --output-on-failure`.
2. Make build and `make check`.
3. Existing library, example, and basic test targets used by those commands.

CMake is the primary verification path. Make remains supported for the same
basic test suite.

## Non-Goals

- Do not support Intel macOS as an explicit target in this phase.
- Do not make FreeBSD or other BSD behavior broader than the existing code
  already intends.
- Do not include `make risk-check` or `make risk-diagnose` in the macOS
  acceptance criteria.
- Do not redesign the coroutine scheduler, hook layer, or public API.
- Do not replace the existing Linux x86 context-switch path.

## Existing Findings

The current tree has partial macOS/BSD support but several Linux assumptions
still leak through:

- CMake declares `cmake_minimum_required(VERSION 2.8)`, which current CMake no
  longer accepts without policy overrides.
- `co_timeout.h` directly includes Linux-only `<sys/epoll.h>`, even though
  `co_epoll.h` already provides a kqueue-backed epoll compatibility layer for
  macOS/BSD.
- The Make build path hard-codes Linux-oriented flags such as `-DLINUX`, `-m64`,
  `-export-dynamic`, and `-ldl`.
- The assembly context switch file only covers i386 and x86_64. Native arm64
  macOS needs a different context strategy.
- macOS exposes deprecated `ucontext` APIs only when the required feature macro
  is defined, and the deprecation warnings conflict with `-Werror`.

## Recommended Approach

Use a minimal native macOS support layer. Keep the existing Linux behavior in
place, use the existing kqueue compatibility code for macOS events, and route
Apple Silicon coroutine context switching through the existing `USE_UCONTEXT`
branch.

This approach is preferred because it matches the requested local target, avoids
new arm64 assembly, and limits the change to build/platform boundaries.

## Alternative Approaches

### CMake-Only Support

This would fix CMake and source platform issues while leaving Make for a later
phase. It would be faster, but it does not satisfy the selected requirement that
both CMake/ctest and `make check` work.

### Full Darwin/BSD Platformization

This would extract a cleaner platform abstraction for Linux, macOS, FreeBSD,
and diagnostics. It is a better long-term direction, but it is too large for
the current "native Apple Silicon macOS" goal.

## Architecture

The design keeps platform differences behind existing boundaries:

- Build files decide which compiler options, libraries, and context backend are
  active for Apple Silicon macOS.
- `co_epoll.h` remains the public/internal compatibility header for epoll-style
  event names and `struct epoll_event`.
- `co_epoll.cpp` remains the implementation switch between Linux epoll and
  macOS/BSD kqueue.
- `routine_context.*` continues to choose between `coctx_swap` and `ucontext`.
  On `__APPLE__ && __aarch64__`, the build selects `USE_UCONTEXT`.

No new public API is added.

## Component Design

### CMake

Update the minimum CMake declaration to a version accepted by current CMake.
Define shared target compile options through CMake rather than appending all
flags globally when practical. On Apple arm64, add the feature macro and warning
suppression needed by `ucontext`, avoid Linux-only link libraries, and avoid
x86-only architecture flags.

### Make

Keep the current Makefile layout and output paths. Add Darwin-aware platform
conditionals in the shared make rules and in top-level/example/test makefiles:

- Do not link `-ldl` on macOS.
- Do not use `-m64` on Apple Silicon.
- Do not define `LINUX` on Darwin.
- Use the same Apple Silicon `ucontext` defines as CMake.

### Event Compatibility

Move Linux-only epoll includes behind `co_epoll.h`. Internal users that need
`epoll_event` should include `co_epoll.h`, not `<sys/epoll.h>` directly.

The existing kqueue-backed implementation remains the macOS event backend. It
should preserve the behavior needed by `co_poll`, including duplicate poll entry
handling and timeout behavior verified by the basic tests.

### Coroutine Context

On native arm64 macOS, do not compile or rely on the existing x86-only assembly
context switch. Select `USE_UCONTEXT` through the build system and make the
headers compile cleanly with AppleClang's requirements for deprecated ucontext
APIs.

Linux and x86 builds keep using the current `coctx_swap` path unless explicitly
configured otherwise.

### Documentation

Update README build notes to mention Apple Silicon macOS support, the verified
commands, and the fact that risk targets remain Linux-oriented for this phase.

## Error Handling

Build failures should remain visible. The build system should not hide compiler
or linker errors with broad suppressions. The only expected macOS-specific
warning suppression is for deprecated `ucontext` declarations when that backend
is selected.

Runtime event registration should preserve platform `errno` behavior where the
existing code already returns system API failures. kqueue allocation or
registration errors should not be swallowed.

## Testing

Primary verification:

```sh
cmake -S . -B build
cmake --build build
cd build && ctest --output-on-failure
```

Secondary verification:

```sh
make check
```

The expected passing coverage is the existing basic suite:

- coroutine create/resume/yield
- async Future/Promise helpers
- duplicate `poll` and direct `co_poll` behavior
- public API smoke test

`make risk-check` and `make risk-diagnose` are excluded from acceptance. Their
Linux-specific diagnostics can be handled in a later design.

## Acceptance Criteria

- CMake configures without policy override flags on the current local macOS
  host.
- CMake builds the library, examples, and basic tests.
- `ctest --output-on-failure` passes.
- `make check` passes.
- Linux-specific source and build behavior is not intentionally changed.
- README documents the macOS target and verification commands.
