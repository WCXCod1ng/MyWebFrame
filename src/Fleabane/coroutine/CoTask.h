//
// Created by user on 2025/12/13.
//

#ifndef COTASK_H
#define COTASK_H
#include <coroutine>
#include <exception>
#include <log/Logger.h>

namespace fleabane {
    /// @brief 协程任务类
    /// 这是所有业务协程函数的返回值类型。
    /// 例如: CoTask handler(Context* ctx) { ... }
    /// 当编译器在编译协程函数时，会生成类似于下面的伪代码
    /// {
    ///     CoTask::promise_type promise; // 创建promise对象
    ///     CoTask task = promise.get_return_object(); // 1. 利用定义的get_return_object()创建 Task
    ///     try {
    ///         co_await promise.initial_suspend(); // 2. 初始挂起（我们选了 never，所以直接往下走）
    ///
    ///         // ... 执行你的业务代码 ...
    ///
    ///     } catch (...) {
    ///         promise.unhandled_exception(); // 3. 异常处理
    ///     }
    ///     co_return; // -> promise.return_void();
    ///     co_await promise.final_suspend(); // 4. 最终挂起（我们选了 never，自动销毁）
    /// }
    ///
    /// 协程的优势不在于“单个请求变快”，而在于**“在资源有限的情况下，能同时处理的请求数量极大增加”**
    struct CoTask {
        struct promise_type;
        using handle_type = std::coroutine_handle<promise_type>;

        // 1. 核心：Promise Type
        // 编译器会利用这个类型来管理协程的状态
        struct promise_type {
            // [必要] 协程创建时调用，生成外层的 CoTask 对象
            CoTask get_return_object() {
                return CoTask{handle_type::from_promise(*this)};
            }

            // [必要] 协程初始化时的行为
            // std::suspend_never 表示协程创建后立即开始执行代码，不暂停
            // 如果由线程池调度，这里也可以改成 suspend_always
            std::suspend_never initial_suspend() { return {}; }

            // [必要] 协程结束时的行为
            // std::suspend_always 表示协程结束后，句柄依然有效（为了获取返回值等），需要手动 destroy
            // std::suspend_never 表示结束后自动销毁句柄
            // 为了安全起见（防止 use-after-free），通常选 never，依靠 RAII 管理
            std::suspend_never final_suspend() noexcept { return {}; }

            // [必要] 协程正常返回 void 时调用 (co_return;)
            void return_void() {}

            // 如果co_return后面跟上其他表达式，我们需要定义return_value函数（注意它与return_void不能并存）

            // [必要] 协程抛出未捕获异常时调用
            void unhandled_exception() {
                try {
                    std::rethrow_exception(std::current_exception());
                } catch (const std::exception& e) {
                    LOG_ERROR("CoTask unhandled exception: {}", e.what());
                } catch (...) {
                    LOG_ERROR("CoTask unhandled unknown exception");
                }
            }
        };

        // --- CoTask 自身的 RAII 管理 ---

        handle_type handle_;

        explicit CoTask(handle_type h) : handle_(h) {}

        ~CoTask() {
            // 如果句柄有效且协程已完成（或者我们需要强制销毁），则销毁它
            if (handle_) {
                // 注意：因为我们在 final_suspend 选了 suspend_never，
                // 协程运行完会自动销毁，这里主要是为了处理异常中断的情况
                // 或者持有句柄进行控制。
                // 对于 Fire-and-Forget 模式，通常 handle_ 只是个弱引用
            }
        }

        // 禁止拷贝，只能移动
        CoTask(const CoTask&) = delete;
        CoTask& operator=(const CoTask&) = delete;
        CoTask(CoTask&& other) noexcept : handle_(other.handle_) {
            other.handle_ = nullptr;
        }
    };
}

#endif //COTASK_H
