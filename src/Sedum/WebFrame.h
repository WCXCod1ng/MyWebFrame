//
// Created by user on 2025/11/30.
//

#ifndef WEBFRAME_H
#define WEBFRAME_H
#include <csignal>
#include <functional>
#include <base/ThreadPool.h>
#include <http/HttpServer.h>

#include "Context.h"
#include "RouterGroup.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
#include "common/Define.h"
#include "WebRouter.h"
#include "common/ConcurrentObjectPool.h"
#include "db/AsyncMysqlPool.h"

namespace sedum {
    using namespace fleabane;

    class RouterGroup;

    class WebFrame {
    public:

        using HandlerFunc = common::HandlerFunc;

        // 我们这里选择方式二，不使用外部的EventLoop，而是自行创建
        WebFrame(const InetAddress& addr, const std::string& name)
            : baseLoop_(name + "#main"),
              eventLoopThreadPool_(std::make_shared<EventLoopThreadPool>(&baseLoop_, 10, "ioLoop")), // 设置业务线程池的线程数为8，最大任务数为1000
              businessPool_(8, 1000, name),
              // server_(&baseLoop_, addr, "ioloop", TcpServer::kReusePort, 0),
              server_(&baseLoop_, addr, name, eventLoopThreadPool_),
              rootGroup_("/", router_) // 管理一个根路由组，它匹配的前缀是“/”
        // 设置ioloop的线程数为8
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
            // 禁止忽略信号（用于调试）
            signal(SIGPIPE, SIG_IGN);
            // 启动HttpServer
            server_.start();
            // 初始化数据库连接池，必须在HttpServer启动之后（实际上是EventLoopThreadLoop启动之后再启动），而且必须在主事件循环之前（否则永远也执行不到）
            AsyncMySQLPool::get_instance().init("127.0.0.1", "root", "wang", "yourdb", 3306, 10, &businessPool_, eventLoopThreadPool_);

            // 启动主事件循环
            baseLoop_.loop();
        }

        // --- 中间件 ---

        /// 注册中间件，实际上是向根路由组上注册
        void use(HandlerFunc middleware) {
            // 实际上是调用根RouterGroup的use
            rootGroup_.use(std::move(middleware));
        }

        /// 创建路由组
        /// @param path 该路由组匹配的前缀（相较于其父路由组）。如果父路由组匹配的前缀是“/root”，那么它实际上匹配的前缀是“/root/{path}”
        RouterGroup group(const std::string& path) {
            return rootGroup_.group(path);
        }

        void GET(const std::string &path, HandlerFunc handler) {
            rootGroup_.GET(path, std::move(handler));
        }
        void POST(const std::string& path, HandlerFunc handler) {
            rootGroup_.POST(path, std::move(handler));
        }
        void PUT(const std::string& path, HandlerFunc handler) {
            rootGroup_.PUT(path, std::move(handler));
        }
        void DELETE(const std::string& path, HandlerFunc handler) {
            rootGroup_.DELETE(path, std::move(handler));
        }
        void HEAD(const std::string& path, HandlerFunc handler) {
            rootGroup_.HEAD(path, std::move(handler));
        }


        /// 设置线程数
        void setThreadNum(const int num) { server_.setThreadNum(num); }


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
        using ExceptionHandler = common::ExceptionHandler;
        void setExceptionHandler(ExceptionHandler handler) {
            exceptionHandler_ = std::move(handler);
        }

        /// 设置RouterGroup为友元类，只允许它能够访问router()函数
        /// 因为WebRouter暴露了add_route的接口，如果用户随意调用，会破坏路由树
        /// 用户无法直接访问WebRouter，只能通过RouterGroup间接访问
        friend class RouterGroup;

    private:

        // 获取路由实例
        WebRouter& router() {
            return router_;
        }


        /// 核心分发逻辑 (Dispatcher)
        /// 这是所有业务的实际入口，会根据路径选择对应的handler并进行处理
        void dispatch(const TcpConnectionPtr& conn, HttpRequest req);

        AsyncVoid doDispatch(std::shared_ptr<Context> ctx);

        static Task<void> defaultNotFoundHandler(const std::shared_ptr<Context>& ctx) {
            ctx->resp().setStatusCode(HttpStatusCode::k404NotFound);
            ctx->resp().setStatusMessage("Not Found");
            ctx->resp().setBody("404 Not Found");
            ctx->resp().setCloseConnection(true);
            co_return;
        }

        static Task<void> defaultMethodNotAllowedHandler(const std::shared_ptr<Context>& ctx) {
            ctx->resp().setStatusCode(HttpStatusCode::k405MethodNotAllowed);
            ctx->resp().setStatusMessage("Method Not Allowed");
            ctx->resp().setBody("405 Method Not Allowed");
            ctx->resp().setCloseConnection(true); // 发生异常通常建议关闭连接
            co_return;
        }

        static Task<void> defaultExceptionHandler(const std::shared_ptr<Context>& ctx, const std::exception& e) {
            ctx->resp().setStatusCode(HttpStatusCode::k500InternalServerError);
            ctx->resp().setBody(std::string("Internal Server Error: ") + e.what());
            ctx->resp().setCloseConnection(true); // 发生异常通常建议关闭连接
            co_return;
        }

        /// 主EventLoop，保证它的生命周期必须最长
        EventLoop baseLoop_;

        std::shared_ptr<EventLoopThreadPool> eventLoopThreadPool_;

        /// 专门用于处理业务的线程池
        fleabane::ThreadPool businessPool_;

        /// 持有底层的 HttpServer
        HttpServer server_;

        /// 路由组件
        WebRouter router_;

        /// 根路由组负责管理全局中间件
        RouterGroup rootGroup_;

        // 保存用户的自定义处理器
        HandlerFunc notFoundHandler_;
        HandlerFunc methodNotAllowedHandler_;
        ExceptionHandler exceptionHandler_;
    };
}


#endif //WEBFRAME_H
