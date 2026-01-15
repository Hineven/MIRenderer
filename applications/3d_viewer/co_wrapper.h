/*
 * Created: 2026/1/15
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_CO_WRAPPER_H
#define MI_CO_WRAPPER_H

#include <coroutine>
#include <utility>
#include <exception>
template<typename T = void>
struct CoTask {
    struct promise_type {
        T result{};
        std::exception_ptr exception{nullptr};

        CoTask get_return_object() {
            return CoTask{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_never initial_suspend() { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }

        void return_value(T value) { result = std::move(value); }

        void unhandled_exception() { exception = std::current_exception(); }
    };

    std::coroutine_handle<promise_type> handle;

    explicit CoTask(std::coroutine_handle<promise_type> h) : handle(h) {}

    ~CoTask() {
        if (handle) handle.destroy();
    }

    // 获取结果
    T get() {
        if (!handle.done()) {
            handle.resume();
        }

        if (handle.promise().exception) {
            std::rethrow_exception(handle.promise().exception);
        }

        return std::move(handle.promise().result);
    }

    // 支持co_await
    bool await_ready() const noexcept {
        return handle.done();
    }

    void await_suspend(std::coroutine_handle<> awaiting_coro) const {
        if (!handle.done()) {
            handle.resume();
        }
        awaiting_coro.resume();
    }

    T await_resume() {
        if (handle.promise().exception) {
            std::rethrow_exception(handle.promise().exception);
        }
        return std::move(handle.promise().result);
    }
};
// void特化版本
template<>
struct CoTask<void> {
    struct promise_type {
        std::exception_ptr exception{nullptr};

        CoTask get_return_object() {
            return CoTask{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_never initial_suspend() { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }

        void return_void() {}
        void unhandled_exception() { exception = std::current_exception(); }
    };

    std::coroutine_handle<promise_type> handle;

    explicit CoTask(std::coroutine_handle<promise_type> h) : handle(h) {}
    ~CoTask() { if (handle) handle.destroy(); }

    void get() {
        if (!handle.done()) {
            handle.resume();
        }
        if (handle.promise().exception) {
            std::rethrow_exception(handle.promise().exception);
        }
    }

    bool await_ready() const noexcept { return handle.done(); }
    void await_suspend(std::coroutine_handle<> awaiting_coro) const {
        if (!handle.done()) {
            handle.resume();
        }
        awaiting_coro.resume();
    }
    void await_resume() {
        if (handle.promise().exception) {
            std::rethrow_exception(handle.promise().exception);
        }
    }
};

#endif //MI_CO_WRAPPER_H