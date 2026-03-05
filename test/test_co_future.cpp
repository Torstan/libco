/*
 * Simplified test for CoFuture/CoPromise
 */
/*

#include "co_routine.h"
#include <iostream>
#include <string>

using namespace co;

void test_producer(void* arg) {
    std::cout << "\n========== producer ==========" << std::endl;
    CoPromise<int> *promise = (CoPromise<int> *)arg;
    // Set value in this coroutine
    promise->set_value(42);
    std::cout << "[Coroutine] Set value to 42" << std::endl;
}

void test_consumer(void* arg) {
    std::cout << "\n========== consumer==========" << std::endl;
    CoPromise<int> *promise = (CoPromise<int> *)arg;
    CoFuture<int> future = promise->get_future();

    // Check if ready - without calling get() which needs event loop
    auto val = future.get();
    std::cout << "get: " << val << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "CoFuture/CoPromise Simple Test" << std::endl;
    std::cout << "========================================" << std::endl;

    // Create and run coroutine
    CoPromise<int> promise;
    Coroutine* co_producer = nullptr, *co_consumer = nullptr;
    co_create(&co, test_consumer, &promise);
    co_resume(co);
    co_create(&co, test_producer, &promise);
    co_resume(co);

    std::cout << "\nTest completed!" << std::endl;
    return 0;
}

*/
