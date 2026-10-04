#pragma once
namespace co {
class Coroutine;
void co_cleanup_sys_envs(void *envs);
namespace detail {
// Borrowed slot owned by this Coroutine; only the hook implementation populates it.
void*& CoroutineEnvs(Coroutine& co);
}
}
