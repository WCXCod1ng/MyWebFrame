//
// Created by user on 2025/11/30.
//

#ifndef CONTEXT_H
#define CONTEXT_H
#include <http/HttpRequest.h>
#include <http/HttpResponse.h>
#include <net/Callbacks.h>
#include <net/TcpConnection.h>


namespace sedum {
    using namespace fleabane;
    class Context;

    using ContextPtr = std::shared_ptr<Context>;

    /// 暴露给业务端用户使用的Context
    class Context {
    public:
        Context(const fleabane::TcpConnectionPtr& conn, HttpRequest&& req,
                std::unordered_map<std::string, std::string> params)
            : conn_(conn), // 引用计数+1
              req_(std::move(req)),
              resp_(true), // 暂时设置为true，稍后会进行修正
              params_(std::move(params)) {

            // 在这里设置，相当于把onRequest的逻辑放到这里执行
            const std::string& connection = req_.getHeader("Connection");
            // 判断是否长连接
            // HTTP/1.1 默认长连接，除非 Connection: close
            // HTTP/1.0 默认短连接，除非 Connection: Keep-Alive
            const bool close = (connection == "close") ||
                               (req_.getVersion() == Version::kHttp10 && connection != "Keep-Alive");
            resp_.setCloseConnection(close);
        }

        /// 获取路径参数 /user/:id -> getParam("id")
        /// 不存在则返回空
        std::optional<std::string> pathVariable(const std::string& key) const {
            if(const auto it = params_.find(key); it != params_.end()) {
                return it->second;
            }
            return std::nullopt;
        }

        /// 获取查询参数 ?name=abc -> query("name") == abc
        std::optional<std::string> query(const std::string& key) const {
            const auto &queries = req_.getQueries();
            if(const auto it = queries.find(key); it != queries.end()) {
                return it->second;
            }
            return std::nullopt;
        }

        /// 获取请求头
        std::optional<std::string> header(const std::string& key) const {
            const auto &headers = req_.headers();
            if(const auto it = headers.find(key); it != headers.end()) {
                return it->second;
            }
            return std::nullopt;
        }


        // 获取原始请求体
        const HttpRequest& req() const { return req_; }
        // 获取原始响应体
        HttpResponse& resp() { return resp_; }


        // --- 响应辅助方法 (类似于) ---

        /// 设置响应头
        void setHeader(const std::string& key, const std::string& value) {
            resp_.addHeader(key, value);
        }

        /// 以字符串方式响应
        void STR(const HttpStatusCode code, const std::string& str) {
            resp_.setStatusCode(code);
            resp_.setContentType("text/plain");
            resp_.setBody(str);
        }

        /// 以json方式响应
        void JSON(const HttpStatusCode code, const std::string& jsonStr) {
            resp_.setStatusCode(code);
            resp_.setContentType("application/json");
            resp_.setBody(jsonStr);
        }

        /// 以对象方式响应
        /// 内部会自动转化为字符串
        template<typename T>
        void OBJ(const HttpStatusCode code, const T& obj) {
            resp_.setStatusCode(code);
            resp_.setContentType("application/json");
            resp_.setBody(obj);
        }

        // HTML, File 等辅助方法...

        // 新增一个关键函数：需要在用户handler调用完毕后通过它来写回数据，实现了HttpServer的后半部分内容
        void flush() {
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
            if (conn_->connected()) {
                Buffer buf;
                resp_.appendToBuffer(&buf);
                conn_->send(&buf); // 线程安全发送

                if (resp_.closeConnection()) {
                    conn_->shutdown();
                }
            }
        }

    private:
        // 为了在执行业务逻辑时使用的不是IO线程，需要更改Context的声明周期（因为在执行业务逻辑时，IO线程已经执行完毕，之前HttpRequest和HttpResponse都是在IO栈上创建的，随着IO线程函数运行完毕，它们都会被释放）
        // const HttpRequest& req_;
        // HttpResponse* resp_;
        const TcpConnectionPtr conn_;
        const HttpRequest req_;
        HttpResponse resp_;
        std::unordered_map<std::string, std::string> params_; // 存储WebRouter解析出来的请求参数，包括路径参数、查询字符串、通配符字符串等
    };
}

#endif //CONTEXT_H
