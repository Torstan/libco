# Libco Code Organization Design

Date: 2026-05-07

## Goal

Improve this repository's code organization while preserving the existing public
API and include paths. The work should make names more intentional, reduce
implementation complexity, and make the documentation describe the repository as
it exists today.

## Constraints

- Keep all existing public headers in their current paths.
- Keep existing public APIs source-compatible, including `co_*` functions,
  `Coroutine`, `Future`, `Promise`, `CoCond`, and `ThreadWorker`.
- Do not require users to change their include directives.
- Do not change scheduler, hook, timeout, poll, epoll, or kqueue behavior.
- Do not perform broad formatting-only rewrites.

## Chosen Approach

Use a compatibility-first layered cleanup.

The public surface remains stable. Existing names and files continue to work.
The implementation becomes clearer through local helper functions, small
internal state structs, and more precise naming inside `.cpp` files. Build files
and documentation are aligned with the current repository contents.

This approach is preferred over a directory-level `detail/` migration because it
delivers the main clarity gains without making file moves or requiring public
compatibility forwarding headers.

## Architecture And Compatibility Boundary

The public layer remains rooted at the existing header files. Basic coroutine
usage continues through `co_routine.h`; async usage continues through
`co_async.h` and `co_future.h`; condition and worker APIs continue through
`co_cond.h` and `thread_worker.h`.

Compatibility fixes should include public declarations for supported functions
that are currently implemented but not declared, such as `co_accept`. Declared
APIs without definitions, such as `Coroutine::Reset()`, must keep their public
declarations and receive definitions. Do not remove those names. The
implementation plan should inspect the current coroutine context and stack
ownership before choosing the exact body, but the design decision is fixed:
resolve declaration and definition mismatches by making the existing API
linkable.

Internal runtime objects such as epoll state, timeout lists, intrusive lists,
stack context, and context switching remain in their current files for this
round. They can be documented as internal concepts, but they are not moved to a
new directory.

## Build And Test Organization

Makefile behavior is the baseline because it currently describes the complete
library better than CMake.

Required build cleanup:

- Align `CMakeLists.txt` with the library sources used by `Makefile`, including
  `co_cond.cpp`, `thread_worker.cpp`, and `routine_context.cpp`.
- Fix CMake example paths to use the `example/` directory.
- Remove missing CMake example targets and list only examples that exist.
- Add current test targets to CMake.
- Remove stale Makefile targets such as `example_exit` when no source file
  exists.
- Add a `make check` entry point that builds and runs the current tests.
- Keep `make all` working.

Build flag cleanup should be conservative. The required work is to document the
existing `v=debug` and `v=release` behavior and avoid adding more duplicated
flags. Centralizing existing flags is out of scope unless it is needed to make
the new `check` target or aligned CMake build work.

## Documentation Design

`README.md` should describe the current repository, not inherited upstream
claims that are not demonstrated here.

Required documentation cleanup:

- State the project purpose and current public API groups.
- Document Makefile and CMake build paths after they are aligned.
- Document `make check`.
- Provide a concise feature matrix that distinguishes implemented and
  demonstrated features from historical or unsupported claims.
- Explain that legacy `co_*` names are kept for compatibility.
- Recommend the C++ API names where appropriate without deprecating legacy
  names.

Add `example/README.md` with a table covering:

- Example binary name.
- Concept demonstrated.
- Required arguments.
- Minimal run command.

The example documentation should also cover cases where an example needs a
server, client, port, or concurrency argument.

## Implementation Cleanup Scope

Only behavior-preserving cleanup is in scope.

### `co_routine.cpp`

Refactor `co_poll_inner()` into clearer internal pieces. The desired reading
order is:

1. Prepare poll state.
2. Register file descriptors.
3. Attach timeout and yield.
4. Copy results back to caller buffers.
5. Clean up registered events and owned memory.

Use an internal state struct or helper functions where that makes ownership and
lifetime clearer. Preserve duplicate file descriptor behavior covered by
`test_co_poll`.

Refactor `co_eventloop()` by extracting helpers for event collection, timeout
collection, and active item dispatch. The event loop should remain behaviorally
equivalent.

### `co_hook_sys_call.cpp`

Extract repeated hook helpers:

- Determine whether a hook should bypass coroutine behavior for a file
  descriptor.
- Convert socket timeout `timeval` values to milliseconds.
- Wait for a file descriptor with a specific poll event mask.
- Share write-loop behavior between `write` and `send` only when errno and
  return-value behavior can be preserved exactly; otherwise extract only the
  common precheck and timeout helpers.
- Share coroutine-local environment lookup for `setenv`, `unsetenv`, and
  `getenv`.

Perform limited `fcntl()` cleanup. The wrapper should make `F_GETFL` and
`F_SETFL` hook behavior easier to audit while preserving existing coverage for
other commands.

### Smaller Files

- In `co_cond.cpp`, extract the repeated waiter activation logic used by
  `Signal()` and `Broadcast()`.
- In `thread_worker.cpp`, improve task coroutine creation and completion
  tracking names.
- In `coctx.cpp`, remove obvious low-risk duplication such as duplicated
  context initialization code if it is identical across architecture branches.

## Out Of Scope

- Moving public headers into `detail/` or `internal/`.
- Removing existing public names.
- Renaming public APIs in place.
- Replacing the epoll or kqueue implementation.
- Changing hook, timeout, poll, scheduling, or coroutine lifetime semantics.
- Adding new functionality beyond build, test, documentation, and cleanup
  support for the existing library.

## Error Handling And Risk Controls

The cleanup should preserve existing error behavior. Where helper extraction is
performed, errno handling, return values, timeout calculations, and cleanup paths
must remain equivalent.

Risk controls:

- Keep each refactor local to one implementation file when possible.
- Prefer helpers with names that describe the existing behavior.
- Avoid changing allocation strategy unless it directly removes unsafe or
  unclear ownership.
- Keep public compatibility wrappers if new clearer aliases are introduced.
- Review every hook wrapper for errno-sensitive behavior after editing.

## Verification

The implementation is complete only after these checks pass:

- `make all`
- `make check`
- CMake configure in a temporary build directory
- CMake build in that directory

Verification must include `test_co_poll` to protect duplicate fd poll behavior,
which was recently fixed.

## Parallel Review Use

Subagents may be used for independent read-only review or for disjoint
implementation areas after the implementation plan is approved. Good candidates
are:

- CMake, Makefile, README, examples, and tests.
- `co_routine.cpp` poll and event loop cleanup.
- `co_hook_sys_call.cpp` hook helper cleanup.

Implementation subtasks must have disjoint write ownership to avoid conflicting
edits.
