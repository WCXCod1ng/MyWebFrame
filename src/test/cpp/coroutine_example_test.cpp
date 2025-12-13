//
// Created by user on 2025/12/12.
//

#include <coroutine>
#include <future>
#include <iostream>
#include <utility>

struct Awaiter {
    int value;

    bool await_ready() {
        // 决定协程是否挂起
        return false;
    }

    // 协程被挂起后执行该函数
    void await_suspend(std::coroutine_handle<> coroutine_handle) {
        // 切换线程
        std::async([=](){
          using namespace std::chrono_literals;
          // sleep 1s
          std::this_thread::sleep_for(1s);
          // 恢复协程
          coroutine_handle.resume();
        });
    }

    // 协程恢复后执行该函数
    int await_resume() {
        // value 将作为 co_await 表达式的值
        return value;
    }
};

struct Result {
    struct promise_type {
        /// Result是协程函数的返回值类型，那么这个Result是如何被创建的呢，实际上是编译器调用get_return_object()来创建的，所以我们需要在这里提前定义好如何创建Result类型
        /// 协程的返回值类型不是在返回之前创建的，而是在协程刚刚被创建之后（协程状态刚被创建之后）就立即构造promise_type对象，并调用get_return_object()来创建这个返回值的
        /// promise_type如何被创建呢？看协程函数的参数，如果能够找到一个构造函数签名与之匹配，则调用这个构造函数，否则调用默认构造函数
        Result get_return_object() {
            return {};
        }

        std::suspend_always initial_suspend() {
            return {};
        }

        /// 当协程代码体中出现“co_return value”，实际上编译器会将其转化为对return_value(xxx)的调用，并且将co_return表达式后面的值value作为其参数传入
        /// 一般来讲，这个value会被存入promise_type的成员中，方便外界访问（这样就实现了协程内外的通信）
        void return_value(int value) {

        }

        // void return_void() {
        //
        // }

        void unhandled_exception() {}

        std::suspend_never final_suspend() noexcept {
            return {};
        }

    };
};

Result Coroutine() {
    std::cout << 1 << std::endl;
    std::cout << co_await Awaiter{.value = 1000} << std::endl;
    std::cout << 2 << std::endl; // 1秒之后再执行
}


struct Generator {

    class ExHaustedException: std::exception {};

    struct promise_type {

        int value; // 将来用于存储结果，以便于外界（Generator）访问
        bool is_ready = false; // 用于判断一个value是否有效

        // 开始执行时不挂起，执行到第一个挂起点
        std::suspend_always initial_suspend() {
            std::cout << "initial_suspend" << std::endl;
            return {};
        };

        // 执行结束后不需要挂起
        /// 协程的状态在协程体执行完之后就会销毁，除非协程挂起在 final_suspend 调用时
        /// 为了让协程的状态的生成周期与 Generator 一致，我们必须将协程的销毁交给 Generator 来处理
        /// 所以这里总是挂起，等到Generator销毁的时候再结束协程（否则后续可能会调用coroutine_handle，导致非法访问）
        std::suspend_always final_suspend() noexcept {
            std::cout << "final_suspend" << std::endl;
            return {};
        }

        // 为了简单，我们认为序列生成器当中不会抛出异常，这里不做任何处理
        void unhandled_exception() { }

        // 构造协程的返回值类型
        Generator get_return_object() {
            std::cout << "create Generator" << std::endl;
            return Generator(std::coroutine_handle<promise_type>::from_promise(*this));
        }

        /// await_transform的作用是为了解决“co_await expr”中的expr不是一个Awaiter的情况：通过这个函数将其转化为一个Awaiter
        /// 另一种思路是为expr这个类型重载operator co_await （但是这里不可行，C++不允许为基本类型重载）
        /// @return 一个Awaiter类型，用于决定是否挂起
        std::suspend_always await_transform(int value) {
            std::cout << "receive value from co_await: " << value <<  std::endl;
            // 在这里可以为成员变量value赋值（实现数据传递）
            this->value = value;
            is_ready = true; // 注意别忘了复制为true（因为外层的Generator只负责判断与置为false）
            return {};
        }

        /// yield_value是专用于co_yield表达式的，本质上所有使用co_yield的地方都能够使用co_await，但是从逻辑上来看，co_yield用于作为生产者（产生数据、生成器）；而co_await则用于作为消费者/等待模型（等待一个操作完成，并拿到操作的结果后继续执行下一步）
        std::suspend_always yield_value(int value) {
            this->value = value;
            is_ready = true;
            return {};
        }

        // 没有返回值
        void return_void() { }
    };

    std::coroutine_handle<promise_type> handle; // 用于操作内部的promise_type

    explicit Generator(std::coroutine_handle<promise_type> handle) noexcept : handle(handle) {}

    Generator(Generator&& generator) noexcept : handle(std::exchange(generator.handle, {})) {}

    /// 关键：禁止Generator的任何复制行为，否则会导致同一个堆区对象被释放两次，或者use-after-free的现象
    Generator(Generator &) = delete;
    Generator& operator=(Generator &) = delete;

    ~Generator() {
        handle.destroy(); // 通过显式调用destroy来销毁协程
    }

    bool has_next() {
        if(!handle || handle.done()) { // 判断协程是否结束
            return false;
        }

        // 到此说明协程没有结束

        // 如果下一个值还没准备好，则恢复协程
        if(!handle.promise().is_ready) {
            handle.resume();
        }

        if(handle.done()) {
            // 如果恢复协程后协程结束，此时必然没有通过co_await传出值来
            return false;
        } else {
            return true;
        }
    }

    int next() {
        if(!has_next()) {
            throw ExHaustedException();
        }
        // 根据has_next的定义，此时is_ready为true，消费它，让它变成false
        handle.promise().is_ready = false;
        // 通过 handle 获取 promise，然后再取到 value
        return handle.promise().value;
    }
};

///  co_await i++; 这一句，我们发现 co_await 后面的是一个整型值，而不是我们在前面的文章当中提到的满足等待体（awaiter）条件的类型，这种情况下该怎么办呢？
/// 实际上，对于 co_await <expr> 表达式当中 expr 的处理，C++ 有一套完善的流程：
/// 1. 如果 promise_type 当中定义了 await_transform 函数，那么先通过 promise.await_transform(expr) 来对 expr 做一次转换，得到的对象称为 awaitable；否则 awaitable 就是 expr 本身。
/// 2. 接下来使用 awaitable 对象来获取等待体（awaiter）。如果 awaitable 对象有 operator co_await 运算符重载，那么等待体就是 operator co_await(awaitable)，否则等待体就是 awaitable 对象本身。
Generator sequence() {
    int i = 0;
    while(true) {
        std::cout << "begin co_await" << std::endl;
        co_yield i++;
    }
}


int main() {
    auto generator = sequence();
    for(int i = 0; i < 5; ++i) {
        if (generator.has_next()) {
            std::cout << generator.next() << std::endl;
        } else {
            break;
        }
    }
    return 0;
}
