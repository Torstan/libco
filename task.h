#pragma once

#include <memory>

namespace co {

class Task {
public:
    virtual ~Task() = default;

    virtual void run() = 0;
};

void schedule(std::unique_ptr<Task> t);
void schedule_urgent(std::unique_ptr<Task> t);

template<typename Func>
class LambdaTask : public Task {
public:
    explicit LambdaTask(const Func& func) : func(func) {}
    LambdaTask(Func&& func) : func(std::move(func)) {}
    void run() override {
        func();
    }
private:
    Func func;
};

template<typename Func>
std::unique_ptr<Task> make_task(Func&& func) {
    return std::make_unique<LambdaTask<Func>>(std::forward<Func>(func));
}

} // namespace co
