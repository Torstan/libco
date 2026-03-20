/*
 * Test for co_async.h - async() function with Future/Promise
 */

#include "co_routine.h"
#include "co_async.h"
#include "thread_worker.h"
#include <iostream>
#include <cassert>
#include <string>

using namespace co;

static int test_passed = 0;
static int test_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (cond) { \
        std::cout << "  PASS: " << msg << std::endl; \
        test_passed++; \
    } else { \
        std::cout << "  FAIL: " << msg << std::endl; \
        test_failed++; \
    } \
} while(0)

// Test 1: async with a simple int-returning lambda
void test_async_basic_int() {
    std::cout << "\n[Test] async basic int" << std::endl;

    Future<int> f = async([]() -> int {
        return 42;
    });

    // The task is scheduled but not yet run. Run the task loop.
    ThreadWorker worker(0);
    worker.run_loop(false);

    TEST_ASSERT(f.available(), "future should be available after run_loop");
    int val = f.get();
    TEST_ASSERT(val == 42, "async returned 42");
}

// Test 2: async with computation
void test_async_computation() {
    std::cout << "\n[Test] async computation" << std::endl;

    Future<int> f = async([]() -> int {
        int sum = 0;
        for (int i = 1; i <= 100; i++) {
            sum += i;
        }
        return sum;
    });

    ThreadWorker worker(0);
    worker.run_loop(false);

    TEST_ASSERT(f.available(), "future should be available");
    int val = f.get();
    TEST_ASSERT(val == 5050, "sum 1..100 == 5050");
}

// Test 3: async with string result
void test_async_string() {
    std::cout << "\n[Test] async string" << std::endl;

    Future<std::string> f = async([]() -> std::string {
        return std::string("hello") + " world";
    });

    ThreadWorker worker(0);
    worker.run_loop(false);

    TEST_ASSERT(f.available(), "future should be available");
    std::string val = f.get();
    TEST_ASSERT(val == "hello world", "async returned 'hello world'");
}

// Test 4: multiple async tasks
void test_async_multiple() {
    std::cout << "\n[Test] multiple async tasks" << std::endl;

    Future<int> f1 = async([]() -> int { return 10; });
    Future<int> f2 = async([]() -> int { return 20; });
    Future<int> f3 = async([]() -> int { return 30; });

    ThreadWorker worker(0);
    worker.run_loop(false);

    TEST_ASSERT(f1.available() && f2.available() && f3.available(),
                "all futures should be available");
    int v1 = f1.get(), v2 = f2.get(), v3 = f3.get();
    TEST_ASSERT(v1 + v2 + v3 == 60, "10 + 20 + 30 == 60");
}

// Test 5: async with exception
void test_async_exception() {
    std::cout << "\n[Test] async exception" << std::endl;

    Future<int> f = async([]() -> int {
        throw std::runtime_error("async error");
        return 0;
    });

    ThreadWorker worker(0);
    worker.run_loop(false);

    TEST_ASSERT(f.available(), "future should be available");
    TEST_ASSERT(f.failed(), "future should be in failed state");

    bool caught = false;
    try {
        f.get();
    } catch (const std::runtime_error& e) {
        caught = true;
        TEST_ASSERT(std::string(e.what()) == "async error", "exception message matches");
    }
    TEST_ASSERT(caught, "exception was caught");
}

// Test 6: async with captured variables
void test_async_capture() {
    std::cout << "\n[Test] async with captured variables" << std::endl;

    int x = 10;
    int y = 20;
    Future<int> f = async([x, y]() -> int {
        return x * y;
    });

    ThreadWorker worker(0);
    worker.run_loop(false);

    TEST_ASSERT(f.available(), "future should be available");
    int val = f.get();
    TEST_ASSERT(val == 200, "10 * 20 == 200");
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "co_async Test Suite" << std::endl;
    std::cout << "========================================" << std::endl;

    test_async_basic_int();
    test_async_computation();
    test_async_string();
    test_async_multiple();
    test_async_exception();
    test_async_capture();

    std::cout << "\n========================================" << std::endl;
    std::cout << "Results: " << test_passed << " passed, " << test_failed << " failed" << std::endl;
    std::cout << "========================================" << std::endl;

    return test_failed > 0 ? 1 : 0;
}
