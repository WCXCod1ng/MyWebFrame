//
// Created by user on 2025/11/30.
//

#include "WebFrame.h"
#include "list"

namespace sedum {
    // 定义线程局部的 Context 池
    // 每个业务线程第一次运行到这里时，会初始化自己的池子
    static thread_local common::ObjectPool<Context> t_contextPool;

    void WebFrame::dispatch(const TcpConnectionPtr &conn, HttpRequest req) {
        // 1. 路由匹配在IO线程中完成
        const auto &path = req.url();
        const auto method = req.method();
        // 调用底层的路由组件查找路由
        auto [status, chain, params] = router_.findRoute(path, method);

        // 2. 确定最终要执行的handler（考虑路由失败的问题）
        HandlersChain targetChain;
        if (status == RouteStatus::FOUND) {
            targetChain = std::move(chain);
        } else if (status == RouteStatus::NOT_FOUND_METHOD) {
            targetChain = {methodNotAllowedHandler_}; // 注意不能使用移动，否则第二次执行到此就会失效
        } else {
            targetChain = {notFoundHandler_};
        }

        // 3. 将所有需要的数据打包进 Lambda，扔进线程池
        // 注意：req 使用 std::move 移动进 lambda
        // conn 是 shared_ptr，拷贝进 lambda 增加引用计数，以防止在执行业务逻辑时，TcpConnection因为关闭而立即析构，使用shared_ptr，可以将它的析构时机延后到最后一个使用点结束
        // params 也是移动
        // chain 是移动，因为我们保证find_route返回的结果是一份拷贝
        businessPool_.enqueue([this,
            conn,
            req = std::move(req),
            chain = std::move(targetChain),
            params = std::move(params)]() mutable
        {
            // --- 以下代码在 业务线程 中执行 ---

            // 4. 创建 Context (在堆上，shared_ptr)
            // 将Middleware的引用传入
            // 将handler移动到Context中，拷贝也可以，但是不能引用，因为handler是栈上的局部变量，dispatch结束后就会析构
            // 构造时 req 再次 move 进 Context
            // 创建Context
            // auto ctx = std::make_shared<Context>();
            auto ctx = t_contextPool.acquire();
            ctx->init(chain, conn, std::move(req), std::move(params));

            // note important!!! 当要实现Fire-and-Forget的异步任务时（也就是协程函数与普通函数的交界处），不要使用复杂的Lambda，因为编译器可能会通过这个Lambda对象的this指针间接访问捕获的对象，而在Fire-and-Forget函数中，常常是启动就结束（而Lambda是存储在栈上的），所以会导致访问捕获对象时出现use-after-free的情况
            doDispatch(ctx);
        });
    }

    AsyncVoid WebFrame::doDispatch(std::shared_ptr<Context> ctx) {
        try {
            co_await ctx->next();
            ctx->flush();
        } catch (const std::exception& e) {
            if (exceptionHandler_) exceptionHandler_(ctx, e);
            ctx->flush();
        }
    }
}
