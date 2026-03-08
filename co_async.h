#pragma once

#include "co_future.h"

template <typename T, typename Func>

class AsyncTask : public Task {
public:
    AsyncTask(Func&& func) : _func(std::move(func)) {}
    void run() override {
        _func();
    }
private:
    Func _func;
};
T async(Func&& func) {
    auto task = make_async_task(std::move(func));
    Future<T> fut = task.get_future();
    schedule(std::move(task));
    return fut.get();
}