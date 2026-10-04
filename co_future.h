#pragma once

#include "internal/wait.h"
#include <cassert>
#include <exception>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <new>

namespace co {

template <typename T>
class Future;

template<typename T>
struct FutureState {
    static constexpr bool copy_noexcept = std::is_nothrow_copy_constructible<T>::value;
    static_assert(std::is_nothrow_move_constructible<T>::value,
                "Types must be no-throw move constructible");
    static_assert(std::is_nothrow_destructible<T>::value,
                "Types must be no-throw destructible");
    static_assert(std::is_nothrow_copy_constructible<std::exception_ptr>::value,
                "std::exception_ptr must be no-throw copy constructible");
    static_assert(std::is_nothrow_move_constructible<std::exception_ptr>::value,
                "std::exception_ptr must be no-throw move constructible");
    enum class State {
        invalid,
        future,
        result,
        exception,
    } _state = State::future;

    union Any {
        Any() noexcept {}
        ~Any() {}
        T value;
        std::exception_ptr ex;
    } _u;
    FutureState() noexcept {}
    FutureState(FutureState&& x) noexcept : _state(x._state) {
        switch (_state) {
            case State::future:
                break;
            case State::result:
                new (&_u.value) T(std::move(x._u.value));
                x._u.value.~T();
                break;
            case State::exception:
                new (&_u.ex) std::exception_ptr(std::move(x._u.ex));
                x._u.ex.~exception_ptr();
                break;
            case State::invalid:
                break;
            default:
                abort();
        }
        x._state = State::invalid;
    }
    ~FutureState() {
        switch (_state) {
            case State::future:
                break;
            case State::result:
                _u.value.~T();
                break;
            case State::exception:
                _u.ex.~exception_ptr();
                break;
            case State::invalid:
                break;
            default:
                abort();
        }
    }
    FutureState& operator=(FutureState&& x) noexcept {
        if (this != &x) {
            this->~FutureState();
            new (this) FutureState(std::move(x));
        }
        return *this;
    }
    bool available() const noexcept {
        return _state == State::result || _state == State::exception;
    }
    bool failed() const noexcept {
        return _state == State::exception;
    }
    void set(const T& value) noexcept {
        assert(_state == State::future);
        new(&_u.value) T(value);
        _state = State::result;
    }
    void set(T&& value) noexcept {
        assert(_state == State::future);
        new(&_u.value) T(std::move(value));
        _state = State::result;
    }
    template <typename... A>
    void set(A&&... a) {
        assert(_state == State::future);
        new(&_u.value) T(std::forward<A>(a)...);
        _state = State::result;
    }
    void set_exception(std::exception_ptr ex) noexcept {
        assert(_state == State::future);
        new(&_u.ex) std::exception_ptr(ex);
        _state = State::exception;
    }
    std::exception_ptr get_exception() && noexcept {
        assert(_state == State::exception);
        _state = State::invalid;
        auto ex = std::move(_u.ex);
        _u.ex.~exception_ptr();
        return ex;
    }
    std::exception_ptr get_exception() const& noexcept {
        assert(_state == State::exception);
        return _u.ex;
    }
    T get_value() && noexcept {
        assert(_state == State::result);
        return std::move(_u.value);
    }
    template<typename U = T>
    std::enable_if_t<std::is_copy_constructible<U>::value, U> get_value() const& noexcept {
        assert(_state == State::result);
        return _u.value;
    }
    T get() && {
        assert(_state != State::future);
        if (_state == State::exception) {
            _state = State::invalid;
            auto ex = std::move(_u.ex);
            _u.ex.~exception_ptr();
            std::rethrow_exception(std::move(ex));
        }
        return std::move(_u.value);
    }
    T get() const& {
        assert(_state != State::future);
        if (_state == State::exception) {
            std::rethrow_exception(_u.ex);
        }
        return _u.value;
    }
};

inline std::exception_ptr make_broken_promise_exception() noexcept {
    try {
        return std::make_exception_ptr(
            std::runtime_error("promise abandoned before setting a result"));
    } catch (...) {
        return std::current_exception();
    }
}

template<typename T>
class Promise {
    Future<T>* _future = nullptr;
    FutureState<T> _local_state;
    FutureState<T>* _state;
    static constexpr bool copy_noexcept = FutureState<T>::copy_noexcept;
public:
    Promise() noexcept : _state(&_local_state) {}
    Promise(Promise&& x) noexcept : _future(x._future), _state(x._state) {
        if (_state == &x._local_state) {
            _state = &_local_state;
            _local_state = std::move(x._local_state);
        }
        x._future = nullptr;
        x._state = nullptr;
        migrated();
    }
    Promise(const Promise&) = delete;
    ~Promise() noexcept {
        abandoned();
    }
    Promise& operator=(Promise&& x) noexcept {
        if (this != &x) {
            this->~Promise();
            new (this) Promise(std::move(x));
        }
        return *this;
    }
    void operator=(const Promise&) = delete;
    Future<T> get_future() noexcept;
    void set_value(const T& result) noexcept(copy_noexcept) {
        do_set_value(result);
    }
    void set_value(T&& result) noexcept {
        do_set_value(std::move(result));
    }
    template <typename... A>
    void set_value(A&&... a) noexcept {
        assert(_state);
        _state->set(std::forward<A>(a)...);
        make_ready();
    }
    void set_exception(std::exception_ptr ex) noexcept {
        do_set_exception(std::move(ex));
    }
    template<typename Exception>
    void set_exception(Exception&& ex) noexcept {
        set_exception(std::make_exception_ptr(std::forward<Exception>(ex)));
    }
private:
    void do_set_value(T result) noexcept {
        assert(_state);
        _state->set(std::move(result));
        make_ready();
    }
    void do_set_exception(std::exception_ptr e) noexcept {
        assert(_state);
        _state->set_exception(std::move(e));
        make_ready();
    }
private:
    void make_ready() noexcept;
    void migrated() noexcept;
    void abandoned() noexcept;

    template<typename U>
    friend class Future;
    friend struct FutureState<T>;
};

template <typename... T> struct is_future : std::false_type {};
template <typename T> struct is_future<Future<T>> : std::true_type {};

struct ready_future_marker {};
struct exception_future_marker {};

template <typename T>
class Future {
    Promise<T>* _promise;
    detail::WaitRecord* _waiter = nullptr;
    FutureState<T> _local_state; //valid if !_promise
    static constexpr bool copy_noexcept = FutureState<T>::copy_noexcept;
private:
    Future(Promise<T>* promise) noexcept : _promise(promise) {
        _promise->_future = this;
    }
    template <typename... A>
    Future(ready_future_marker, A&& ...args) : _promise(nullptr) {
        _local_state.set(std::forward<A>(args)...);
    }
    template <typename... A>
    Future(exception_future_marker, std::exception_ptr ex) noexcept : _promise(nullptr) {
        _local_state.set_exception(std::move(ex));
    }
    explicit Future(FutureState<T>&& state) noexcept
        : _promise(nullptr), _local_state(std::move(state)) {
    }
    FutureState<T>* state() noexcept {
        return _promise ? _promise->_state : &_local_state;
    }
    FutureState<T> get_available_state() noexcept {
        auto st = state();
        if (_promise) {
            _promise->_future = nullptr;
            _promise = nullptr;
        }
        return std::move(*st);
    }
public:
    using value_type = T;
    using promise_type = Promise<T>;
    Future(Future&& x) noexcept : _promise(x._promise) {
        // An object with an active member call must remain alive and stationary.
        if (x._waiter) std::terminate();
        if (!_promise) {
            _local_state = std::move(x._local_state);
        }
        x._promise = nullptr;
        if (_promise) {
            _promise->_future = this;
        }
    }
    ~Future() {
        if (_waiter) std::terminate();
        if (_promise) {
            _promise->_future = nullptr;
        }
    }
    Future(const Future&) = delete;
    void operator=(const Future&) = delete;
    Future& operator=(Future&& x) noexcept {
        if (this != &x) {
            this->~Future();
            new (this) Future(std::move(x));
        }
        return *this;
    }
    T get() {
        if (_waiter) throw std::logic_error("Future already has a waiter");
        if (!state()->available()) {
            wait();
        }
        return get_available_state().get();
    }
    std::exception_ptr get_exception() {
        if (_waiter) throw std::logic_error("Future already has a waiter");
        if (!_promise && !_local_state.available()) {
            throw std::logic_error("Future has no result state");
        }
        return get_available_state().get_exception();
    }
    void wait() {
        if (_waiter) throw std::logic_error("Future already has a waiter");
        if (state()->available()) {
            return;
        }
        if (!_promise) throw std::logic_error("Future has no result state");
        detail::WaitRecord wait;
        _waiter = &wait;
        wait.SetCleanup([](void* ptr) noexcept {
            static_cast<Future*>(ptr)->_waiter = nullptr;
        }, this);
        wait.Suspend();
    }
    bool available() noexcept {
        return state()->available();
    }
    bool failed() noexcept {
        return state()->failed();
    }
    template<typename U>
    friend class Promise;
    template <typename U, typename A>
    friend Future<U> make_ready_future(A&& value);
    template <typename U>
    friend Future<U> make_exception_future(std::exception_ptr ex) noexcept;
    template <typename U, typename Exception>
    friend Future<U> make_exception_future(Exception&& ex) noexcept;
};

template <typename T>
inline Future<T> Promise<T>::get_future() noexcept {
    assert(!_future && _state);
    return Future<T>(this);
}

template <typename T>
inline void Promise<T>::make_ready() noexcept {
    if (_future && _future->_waiter) _future->_waiter->Complete();
}

template <typename T>
inline void Promise<T>::migrated() noexcept {
    if (_future) {
        _future->_promise = this;
    }
}

template <typename T>
void Promise<T>::abandoned() noexcept {
    if (_future) {
        assert(_state);
        if (!_state->available()) {
            _state->set_exception(make_broken_promise_exception());
        }
        _future->_local_state = std::move(*_state);
        _future->_promise = nullptr;
        _state = nullptr;
        if (_future->_waiter) _future->_waiter->Complete();
    }
}

template <typename T, typename A>
inline Future<T> make_ready_future(A&& value) {
    return Future<T>(ready_future_marker(), std::forward<A>(value));
}

template <typename T>
inline Future<T> make_exception_future(std::exception_ptr ex) noexcept {
    return Future<T>(exception_future_marker(), std::move(ex));
}

template <typename T, typename Exception>
inline Future<T> make_exception_future(Exception&& ex) noexcept {
    return make_exception_future<T>(std::make_exception_ptr(std::forward<Exception>(ex)));
}

} // namespace co
