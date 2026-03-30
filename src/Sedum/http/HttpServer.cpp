//
// Created by user on 2025/11/29.
//

#include "HttpServer.h"

#include "HttpContext.h"
#include "HttpResponse.h"
#include "log/Logger.h"
#include "net/TcpConnection.h"

namespace sedum {

    /// 这里不能简单地直接使用无锁的对象池，因为虽然TcpConnection是IO线程唯一的，但是TcpServer会管理多个TcpConnection，这意味着它提供的回调可能会被多个线程并发执行，
    /// 相应地，HttpServer也是存在多个线程并发执行其中的函数的情况，所以我们要加锁。
    /// 或者可以使用另一种方式：因为IO线程自从服务启动后就不会被销毁和新建，所以我们完全可以创建一个线程局部的对象池，这种情况下也能使用无锁的对象池
    static thread_local common::ObjectPool<HttpContext> t_httpContextPool;

    // 默认的 HTTP 回调（如果用户没设置）
    void defaultHttpCallback(const TcpConnectionPtr& conn, HttpRequest request) {
        HttpResponse resp {true};
        resp.setStatusCode(HttpStatusCode::k404NotFound);
        resp.setStatusMessage("Not Found");
        resp.setCloseConnection(true);
    }

    HttpServer::HttpServer(EventLoop *loop,
                   const InetAddress &listenAddr,
                   const std::string &name,
                   const std::shared_ptr<EventLoopThreadPool>& eventLoopThreadPool,
                   const TcpServer::Option option,
                   const size_t numThreads,
                   const double idleTimeoutSeconds)
                       :server_(loop, listenAddr, name, eventLoopThreadPool, option, numThreads, idleTimeoutSeconds),
                        httpCallback_(defaultHttpCallback)
    {
        // 注册 TcpServer 的回调
        // 1. 连接建立/断开时 -> onConnection
        server_.setConnectionCallback(
            std::bind(&HttpServer::onConnection, this, std::placeholders::_1));

        // 2. 收到数据时 -> onMessage
        server_.setMessageCallback(
            std::bind(&HttpServer::onMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
    }

    HttpServer::~HttpServer() {
    }

    void HttpServer::start() {
        LOG_INFO("HttpServer[{}] starts listening on {}", server_.name(), server_.ipPort());
        server_.start();
    }

    void HttpServer::onConnection(const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            LOG_INFO("Connection UP : {}", conn->peerAddress().toIpPort());

            // 1. 为每个新连接创建一个 HttpContext
            // HttpContext 内部包含状态机和 HttpRequest 对象
            // 使用 std::any (setContext) 绑定到 TcpConnection 上
            conn->setContext(t_httpContextPool.acquire());

            // // 2. 【关键】启动协程
            // // handleHttpSession 返回一个 CoTask 对象。
            // // 由于 CoTask 的 promise_type 设置为 initial_suspend = never，
            // // 调用该函数时，协程代码会立即开始执行，直到遇到第一个 co_await recv。
            // // 协程的状态机分配在堆上，即使 handleHttpSession 返回，协程依然存活。
            // handleHttpSession(conn);
        } else {
            LOG_INFO("Connection DOWN : {}", conn->peerAddress().toIpPort());
        }
    }

    CoTask HttpServer::handleHttpSession(TcpConnectionPtr conn) {
        // 1. 取出该连接对应的解析器 (Context)
        // 此时 context 可能是“全新的”，也可能是“解析了一半的”（针对分包情况）
        const auto context = conn->getContextValue<std::shared_ptr<HttpContext>>();

        // 准备缓冲区（可以使用 conn 自带的 inputBuffer，也可以用局部 Buffer）
        // 这里假设我们定义一个局部的 Buffer 用于接收数据。
        Buffer buf;

        // 现在我们主动模拟HttpContext的状态机（之前是通过onMessage进行模拟）
        while(true) {
            // 1. 【挂起】主动拉取数据
            // 如果没有数据，协程挂起，控制权回到 EventLoop
            // 如果有数据（或出错），协程恢复
            ssize_t n = co_await conn->recv(&buf);

            if (n > 0) {
                // important!!! 有数据，就相当于之前的onMessage被调用了
                // 2. 解析 HTTP
                // parseRequest 逻辑与 onMessage 类似，但这里是在协程上下文中
                if (!context->parseRequest(&buf, TimeStamp::now())) {
                    conn->send("HTTP/1.1 400 Bad Request\r\n\r\n");
                    conn->forceClose();
                    co_return; // 退出协程
                }

                if (context->gotAll()) {
                    // 3. 分发请求
                    // 将 Request 移交给业务层 (WebFrame)
                    // 这里调用 onRequest，它内部会调用 httpCallback_ -> WebFrame::dispatch
                    // 此时代码依然运行在 IO 线程中
                    onRequest(conn, std::move(context->request()));

                    // 4. 重置状态机，准备处理下一个 Keep-Alive 请求
                    context->reset();
                }
            }
            else if (n == 0) {
                // important!!! 这里实际上模拟的是回调方式中TcpConnection::handleChannelClose()被触发
                // 对端关闭
                conn->forceClose(); // 触发清理流程
                break;
            }
            else {
                // important!!! 这里实际上模拟的是回调方式中TcpConnection::handleChannelError()被触发
                // 出错
                conn->forceClose();
                break;
            }
        }

        co_return;
    }

    void HttpServer::onMessage(const TcpConnectionPtr& conn, Buffer* buf, TimeStamp receiveTime) {
        // 1. 取出该连接对应的解析器 (Context)
        // 此时 context 可能是“全新的”，也可能是“解析了一半的”（针对分包情况）
        const auto context = conn->getContextValue<std::shared_ptr<HttpContext>>();

        // 2. 尝试解析 Buffer 中的数据
        if (!context->parseRequest(buf, receiveTime)) {
            // 解析失败（数据不够，或者格式错误）
            // 这里的 parseRequest 返回 false 只有一种可能：
            // 格式错误 -> 发送 400
            // 因为parseRequest内部会将数据不全认为正常（它会记录上一次请求的状态）

            // 为了简化，我们假设 parseRequest 内部处理了断点续传。
            // 如果这里直接返回 false，通常意味着请求格式是错误的（Bad Request）。
            conn->send("HTTP/1.1 400 Bad Request\r\n\r\n");
            conn->shutdown();
        }

        // 3. 检查是否解析完了整个请求 (GotAll)
        if (context->gotAll()) {
            // 【关键修改】
            // 解析完成，将 Request 从 Context 中“偷”出来 (move)，移交给 onRequest
            // 这里串引用，不会增加shared_ptr的计数
            onRequest(conn, std::move(context->request()));

            // 【关键】重置 Context 状态机
            // 因为是 Keep-Alive，连接不会断，后续可能还有新的请求发过来
            context->reset();
        }
    }

    void HttpServer::onRequest(const TcpConnectionPtr& conn, HttpRequest req) const {
        // const std::string& connection = req.getHeader("Connection");
        // // 判断是否长连接
        // // HTTP/1.1 默认长连接，除非 Connection: close
        // // HTTP/1.0 默认短连接，除非 Connection: Keep-Alive
        // bool close = (connection == "close") ||
        //              (req.getVersion() == Version::kHttp10 && connection != "Keep-Alive");

        // 我们不再这里创建 HttpResponse，也不在这里发送。
        // 我们把 Request 所有权和 Connection 指针，以及“是否需要关闭连接”的建议
        // 全部打包传给回调函数。

        // 注意：我们将 close 标志暂存到 Request 的上下文中，或者简单点，
        // 让 WebFrame 重新判断一次（开销很小），这里为了接口简洁，我们只传 req。

        // 调用用户回调 (业务逻辑)
        // 用户填充 response 的状态码、Header、Body
        if(httpCallback_) {
            httpCallback_(conn, std::move(req));
        }

        // 注意，序列化相应并通过网络发回的操作也不会在这里完成，而是将来在线程池中完成
        // // 序列化响应并通过网络发送
        // Buffer buf;
        // response.appendToBuffer(&buf);
        // conn->send(&buf); // 此时数据进入 TcpConnection 的 OutputBuffer
        //
        // // 如果需要关闭连接，调用 shutdown
        // // 注意：TcpConnection::shutdown 会等待数据发完再关闭
        // if (response.closeConnection()) {
        //     conn->shutdown();
        // }
    }
}