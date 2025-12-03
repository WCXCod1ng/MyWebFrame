//
// Created by user on 2025/11/30.
//

#ifndef WEBFRAME_H
#define WEBFRAME_H
#include <functional>
#include <base/ThreadPool.h>
#include <http/HttpServer.h>

#include "Context.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"

#include "WebRouter.h"

namespace sedum {
    using namespace fleabane;

    class WebFrame {
    public:
        // WebFrame(EventLoop* loop, const InetAddress& addr, const std::string& name)
        //     : server_(loop, addr, name)
        // {
        //     // 将框架的 dispatch 方法注册给底层 HttpServer
        //     server_.setHttpCallback(
        //         std::bind(&WebFrame::dispatch, this, std::placeholders::_1, std::placeholders::_2));
        // }

        // 我们这里选择方式二，不使用外部的EventLoop，而是自行创建
        WebFrame(const InetAddress& addr, const std::string& name)
          : baseLoop_(),
            businessPool_(8, 1000, name), // 设置业务线程池的线程数为8，最大任务数为1000
            server_(&baseLoop_, addr, "ioloop") // 设置ioloop的线程数为8
        {
            // WebFrame在主线程中被构造
            current_thread::set_name(name + "#main");

            // 启动业务线程池，在我们的实现中，ThreadPool一旦被创建就会自行启动
            // businessPool_.start();

            server_.setHttpCallback(
                std::bind(&WebFrame::dispatch, this, std::placeholders::_1, std::placeholders::_2));

            // 初始化默认处理函数
            notFoundHandler_ = defaultNotFoundHandler;
            methodNotAllowedHandler_ = defaultMethodNotAllowedHandler;
            exceptionHandler_ = defaultExceptionHandler;
        }

        /// 启动服务
        void start() {
            // 启动HetpServer
            server_.start();
            // 启动主事件循环
            baseLoop_.loop();
        }

        /// 设置线程数
        void setThreadNum(const int num) { server_.setThreadNum(num); }

        // --- 路由注册接口 ---

        void GET(const std::string& path, HandlerFunc handler) {
            router_.add_route(path, Method::kGet, std::move(handler));
        }

        void POST(const std::string& path, HandlerFunc handler) {
            router_.add_route(path, Method::kPost, std::move(handler));
        }

        void PUT(const std::string& path, HandlerFunc handler) {
            router_.add_route(path, Method::kPut, std::move(handler));
        }

        void DELETE(const std::string& path, HandlerFunc handler) {
            router_.add_route(path, Method::kDelete, std::move(handler));
        }

        void HEAD(const std::string& path, HandlerFunc handler) {
            router_.add_route(path, Method::kHead, std::move(handler));
        }


        /// 自定义 404 处理
        void setNotFoundHandler(HandlerFunc handler) {
            notFoundHandler_ = std::move(handler);
        }

        /// 自定义 405 处理
        void setMethodNotAllowedHandler(HandlerFunc handler) {
            methodNotAllowedHandler_ = std::move(handler);
        }

        /// 自定义全局异常处理
        /// 注意：异常处理器的签名多了一个 exception 参数
        using ExceptionHandler = std::function<void(Context&, const std::exception&)>;
        void setExceptionHandler(ExceptionHandler handler) {
            exceptionHandler_ = std::move(handler);
        }

    private:

        /// 核心分发逻辑 (Dispatcher)
        /// 这是所有业务的实际入口，会根据路径选择对应的handler并进行处理
        void dispatch(const fleabane::TcpConnectionPtr& conn, fleabane::HttpRequest req) {
            // 1. 路由匹配在IO线程中完成
            const auto &path = req.url();
            const auto method = req.method();
            // 调用底层的路由组件
            auto [status, handler, params] = router_.find_route(path, method);

            // 2. 将所有需要的数据打包进 Lambda，扔进线程池
            // 注意：req 使用 std::move 移动进 lambda
            // conn 是 shared_ptr，拷贝进 lambda 增加引用计数
            // params 也是移动
            // handler 是拷贝，因为不能更改router的内容
            businessPool_.enqueue([this, conn, req = std::move(req), status = status,
                                 handler = handler, params = std::move(params)]() mutable
            {
                // --- 以下代码在 业务线程 中执行 ---

                // 3. 创建 Context (在堆上，shared_ptr)
                // 构造时 req 再次 move 进 Context
                const auto ctx = std::make_shared<Context>(conn, std::move(req), std::move(params));

                // 4. 执行 Handler
                if (status == RouteStatus::FOUND) {
                    try {
                        handler(*ctx); // 传引用
                    } catch (const std::exception& e) {
                        if (exceptionHandler_) exceptionHandler_(*ctx, e);
                    }
                } else if (status == RouteStatus::NOT_FOUND_METHOD) {
                    methodNotAllowedHandler_(*ctx);
                } else {
                    notFoundHandler_(*ctx);
                }

                // 5. 发送响应
                // 业务逻辑执行完后，主动将 Response 写回
                ctx->flush();
            });
        }

        static void defaultNotFoundHandler(Context& ctx) {
            ctx.resp().setStatusCode(HttpStatusCode::k404NotFound);
            ctx.resp().setStatusMessage("Not Found");
            ctx.resp().setBody("404 Not Found");
            ctx.resp().setCloseConnection(true);
        }

        static void defaultMethodNotAllowedHandler(Context& ctx) {
            ctx.resp().setStatusCode(HttpStatusCode::k405MethodNotAllowed);
            ctx.resp().setStatusMessage("Method Not Allowed");
            ctx.resp().setBody("405 Method Not Allowed");
            ctx.resp().setCloseConnection(true); // 发生异常通常建议关闭连接
        }

        static void defaultExceptionHandler(Context& ctx, const std::exception& e) {
            ctx.resp().setStatusCode(HttpStatusCode::k500InternalServerError);
            ctx.resp().setBody(std::string("Internal Server Error: ") + e.what());
            ctx.resp().setCloseConnection(true); // 发生异常通常建议关闭连接
        }

        /// 主EventLoop，保证它的生命周期必须最长
        EventLoop baseLoop_;

        /// 专门用于处理业务的线程池
        fleabane::ThreadPool businessPool_;

        /// 持有底层的 HttpServer
        HttpServer server_;

        /// 路由组件
        WebRouter router_;

        // 保存用户的自定义处理器
        HandlerFunc notFoundHandler_;
        HandlerFunc methodNotAllowedHandler_;
        ExceptionHandler exceptionHandler_;
    };
}


#endif //WEBFRAME_H
