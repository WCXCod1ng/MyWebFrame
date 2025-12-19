//
// Created by user on 2025/12/15.
//

#ifndef COROUTINE_H
#define COROUTINE_H
#include <coroutine>
#include <exception>


namespace sedum {
    // ========================================================
    // 1. Task<T>: 用于业务逻辑 interconnect (Middleware/Handler)
    // ========================================================
    template <typename T = void>
    class Task {
    public:
        struct promise_type;
        using handle_type = std::coroutine_handle<promise_type>;

        struct promise_type {
            T result_; // 存储返回值
            std::exception_ptr exception_; // 存储异常

            /// 需要存储调用者协程的句柄，方便协程执行结束后唤醒caller协程
            std::coroutine_handle<> continuation_;

            Task get_return_object() {
                return Task(handle_type::from_promise(*this));
            }

            /// 初始挂起：suspend_always
            /// 意味着调用函数时，协程创建但不立即执行。只有当外层 co_await task 时，才开始执行。
            /// 当调用 auto task = next(); 时，协程帧创建了，但代码一行都没跑。只有当 co_await task; 时，它才开始跑。
            /// 好处：这非常符合 C++ 的同步编程直觉，同时也防止了如果你获取了 task 但丢弃它导致的资源浪费或逻辑错误
            /// 这对于构建洋葱模型至关重要，我们希望由 next() 控制执行时机。
            std::suspend_always initial_suspend() { return {}; }

            /// 注意，当协程执行结束时（标志是final_suspend()被调用），需要保证控制流转移回父协程函数，就在这个FinalAwaiter中实现
            struct FinalAwaiter {
                bool await_ready() noexcept { return false;}
                /// 核心，在这里进行控制流切换，从当前的协程切换回调用者
                /// ======= 重要：必须使用对称转移 ======
                /// 1. A 执行到 co_await B()。A 挂起。
                /// 2. B 开始执行。
                /// 3. B 执行完毕，进入 final_suspend。
                /// 4. 问题爆发点： 在我们之前的代码中，FinalAwaiter::await_suspend 是这样写的：
                /// ```c++
                /// void await_suspend(handle_type h) {
                /// auto parent = h.promise().continuation_;
                /// if (parent) parent.resume(); // <--- 致命的函数调用！
                /// }
                /// ```
                /// 5. parent.resume() 被调用，A 立即恢复执行。注意：此时 B 的 await_suspend 函数还没有返回，B 的栈帧还“活”在调用栈上。
                /// 6. A 从 co_await B() 处醒来，继续往下走。
                /// 7. A 的局部变量 Task b_task 离开作用域，触发析构函数 ~Task()。
                /// 8. ~Task() 调用 b_handle.destroy()。B 的协程帧被销毁了。
                /// 9. 回马枪： A 继续执行或返回。但此时，原本的函数调用 parent.resume() 终于结束了，程序试图回到 B 的 await_suspend 函数中继续执行后续指令（比如函数返回）。
                /// 10. 崩溃： 此时 B 的内存空间已经被销毁了（第 8 步），CPU 跑到了一个不存在的内存地址或脏数据上 -> SIGILL。
                ///
                /// 解决方案是：将await_suspend的返回值类型改为std::coroutine_handle<>，并且将原来的parent.resume()更改为return parent，意思就是说控制流回到parent所对应的协程函数
                std::coroutine_handle<> await_suspend(handle_type h) noexcept {
                    auto parent = h.promise().continuation_;
                    // // 如果有调用者协程，就调用它
                    // if (parent) {
                    //     parent.resume();
                    // }
                    if(parent) {
                        return parent;
                    }
                    return std::noop_coroutine(); // 如果没人等，就停下来
                }
                void await_resume() noexcept {}
            };
            FinalAwaiter final_suspend() noexcept { return {}; }

            /// 捕获协程执行过程中抛出的异常
            void unhandled_exception() {
                exception_ = std::current_exception();
            }

            /// 处理返回值
            /// co_return value会传入这里
            template<typename U>
            void return_value(U&& value) {
                result_ = std::forward<U>(value);
            }
        };

        handle_type handle_;

        explicit Task(handle_type h) : handle_(h) {}

        // 禁止拷贝，只能移动（RAII）
        Task(const Task&) = delete;
        Task(Task&& other) noexcept : handle_(other.handle_) {
            other.handle_ = nullptr;
        }

        // Task 销毁时，连带销毁协程 Frame
        ~Task() {
            if (handle_) handle_.destroy();
        }

        // note 通常我们需要传递 co_await，所以应当把Task也设计成Awaitable的

        // Awaitable 接口实现
        bool await_ready() { return false; }

        /// 当 co_await this 时调用
        /// 需要把父协程的handle存储下来
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) {
            // 1. 记录下哪个协程调用我
            handle_.promise().continuation_ = caller;
            // // 2. 执行我
            // handle_.resume();
            return handle_; // 跳转到本Task代表的协程
        }

        /// 当协程恢复并执行完毕后，调用此函数获取结果
        /// 这里要将异常重新抛出，以便于能够获取业务handler抛出的异常
        /// 当写下auto user = co_await db.queryUser()代码时，编译器实际上把它展开成了三步：
        /// 1. 获取 Awaiter：拿到 Task 对象。
        /// 2. 挂起/恢复：调用 await_suspend，挂起，等待，然后恢复。
        /// 3. 获取结果（关键点）： 调用 awaiter.await_resume()。
        ///     - 如果 Promise 里有异常：await_resume 会 rethrow，异常会炸出来。所以即使忽略返回值，异常检测依然有效。
        ///     - 如果正常返回：await_resume 返回 User 对象（临时变量）。
        ///     - 将User对象赋值给user，如果没有使用变量承接，则立即析构
        T await_resume() {
            if (handle_.promise().exception_) {
                std::rethrow_exception(handle_.promise().exception_);
            }
            return handle_.promise().result_;
        }
    };

    // 针对 void 的特化，去掉 result_ 存储
    template <>
    class Task<void> {
    public:
        struct promise_type;
        using handle_type = std::coroutine_handle<promise_type>;

        struct promise_type {
            std::exception_ptr exception_;
            // 记录父协程
            std::coroutine_handle<> continuation_;
            Task get_return_object() { return Task(handle_type::from_promise(*this)); }
            std::suspend_always initial_suspend() { return {}; }

            // 修改
            struct FinalAwaiter {
                bool await_ready() noexcept { return false;}
                std::coroutine_handle<> await_suspend(handle_type h) noexcept {
                    auto parent = h.promise().continuation_;
                    // // 如果有调用者协程，就调用它
                    // if (parent) {
                    //     parent.resume();
                    // }
                    if(parent) {
                        return parent;
                    }
                    return std::noop_coroutine(); // 如果没人等，就停下来
                }
                void await_resume() noexcept {}
            };
            FinalAwaiter final_suspend() noexcept { return {}; }

            void unhandled_exception() { exception_ = std::current_exception(); }
            void return_void() {} // 对应 co_return;
        };

        handle_type handle_;
        explicit Task(handle_type h) : handle_(h) {}
        Task(const Task&) = delete;
        Task(Task&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
        ~Task() {
            if (handle_) {
                handle_.destroy();
                handle_ = nullptr;
            }
        }

        bool await_ready() { return false; }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) {
            // 1. 记录谁调用了我
            handle_.promise().continuation_ = caller;
            // // 2. 执行我自己
            // handle_.resume();
            return handle_; // 跳转到本Task所代表的协程
        }

        void await_resume() {
            if (handle_.promise().exception_) {
                std::rethrow_exception(handle_.promise().exception_);
            }
        }
    };

    // ========================================================
    // 2. AsyncVoid: 用于入口点 (Fire-and-Forget)
    // ========================================================
    // 这是一个特殊的类型，用于在普通函数（线程池 Lambda）中启动一个协程。
    // 它的特点是：调用即运行，自我销毁。
    struct AsyncVoid {
        struct promise_type {
            AsyncVoid get_return_object() { return {}; }

            // 初始不挂起：suspend_never
            // 创建即运行，不需要 co_await。
            std::suspend_never initial_suspend() { return {}; }

            // 结束不挂起：suspend_never
            // 协程运行结束后，自动销毁 Frame。
            // 因为外层没有 Task 对象持有它的 Handle（没人等它），所以必须自杀。
            // 我们在 dispatch 的 Lambda 里调用 run_pipeline()；Lambda 是普通函数，不能 co_await；所以协程必须自己启动（Eager Start）。
            // 同时，Lambda 执行完就退出了，没有对象能持有协程的 Handle，所以协程必须在做完所有事情后自己销毁（final_suspend = suspend_never）
            std::suspend_never final_suspend() noexcept { return {}; }

            void return_void() {}

            // 这里的异常通常是“顶层未捕获异常”，通常应该 log 下来
            void unhandled_exception() {
                // LOG_INFO("Uncaught exception in AsyncTask");
                std::terminate();
            }
        };
    };
}



#endif //COROUTINE_H
