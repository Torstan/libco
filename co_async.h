#pragma once

#include "co_future.h"
#include <type_traits>
#include <memory>

/// async: run a callable asynchronously via the task scheduler, return a Future<T>
///
/// The callable is wrapped as a Task and scheduled for execution.
/// The returned Future<T> can be used to retrieve the result (blocking via
/// coroutine yield if the result is not yet available).
///
/// Usage:
///   Future<int> f = async([]() { return 42; });
///   int val = f.get();  // blocks (yields) until the task completes

template <typename Func,
          typename T = std::invoke_result_t<std::decay_t<Func>>,
          std::enable_if_t<!std::is_void_v<T>, int> = 0>
Future<T> async(Func&& func) {
    auto promise = std::make_unique<Promise<T>>();
    Future<T> future = promise->get_future();

    auto task = make_task([p = std::move(promise), f = std::decay_t<Func>(std::forward<Func>(func))]() mutable {
        try {
            p->set_value(f());
        } catch (...) {
            p->set_exception(std::current_exception());
        }
    });
    schedule(std::move(task));
    return future;
}
