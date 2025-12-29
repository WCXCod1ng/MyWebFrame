//
// Created by user on 2025/11/30.
//

#ifndef CONTEXT_H
#define CONTEXT_H
#include <WebRouter.h>
#include <http/HttpRequest.h>
#include <http/HttpResponse.h>
#include <net/Callbacks.h>
#include <net/TcpConnection.h>
#include "common/Define.h"
#include "common/JsonUtil.h"
#include "db/DBInterfaces.h"

namespace sedum {
    using namespace fleabane;
    class Context;

    using ContextPtr = std::shared_ptr<Context>;

    /// 暴露给业务端用户使用的Context
    class Context : public std::enable_shared_from_this<Context> { // 注意一定要使用public
    public:
        using HandlerFunc = common::HandlerFunc;

        Context() : resp_(true) {}

        Context(std::vector<HandlerFunc> handlersChain, const TcpConnectionPtr& conn, HttpRequest&& req,
                std::unordered_map<std::string, std::string> params)
            : handlersChain_(std::move(handlersChain))
              ,conn_(conn), // 引用计数+1
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

        /// 由于对象池只能调用默认构造，所以需要提供一个方法来初始化成员变量
        void init(std::vector<HandlerFunc> handlersChain, const TcpConnectionPtr& conn, HttpRequest&& req,
                std::unordered_map<std::string, std::string> params) {
            handlersChain_ = std::move(handlersChain);
            conn_ = conn; // 引用计数+1
            req_ = std::move(req);
            params_ = std::move(params);

            index_ = -1;
            resp_.reset();
            variables_.clear();

            // 在这里设置，相当于把onRequest的逻辑放到这里执行
            const std::string& connection = req_.getHeader("Connection");
            // 判断是否长连接
            // HTTP/1.1 默认长连接，除非 Connection: close
            // HTTP/1.0 默认短连接，除非 Connection: Keep-Alive
            const bool close = (connection == "close") ||
                               (req_.getVersion() == Version::kHttp10 && connection != "Keep-Alive");
            resp_.setCloseConnection(close);
        }

        // --- 实现中间件功能 ---

        /// 驱动执行下一个Handler
        /// 这实现了与Gin一致的洋葱模型
        /// 引入协程后，它每次推进一步，都需要等待相应的handler执行
        Task<void> next() {
            index_++; // 更新handler执行链的状态

            // 这里没有像Gin那样通过一个循环管理，而是使用if
            // 原因在于我们希望Middleware显式调用next才会传递，如果调用则递归会继续推进index_，不调用则会退出
            // 对于中间件：需要主动next，否则认为中断
            // 对于业务handler，由于它是最后一个，所以即使调用next也会退出

            if(index_ < static_cast<int>(handlersChain_.size())) {
                // 说明没有到结束（后续还有handler），执行它
                co_await handlersChain_[index_](shared_from_this());
                LOG_INFO("continue");
            }
            co_return;
        }

        /// 显式终止后续处理
        void abort() {
            index_ = static_cast<int>(handlersChain_.size());
        }

        /// 判断是否终止，Middleware和handler都执行完毕意味着要终止
        bool isAborted() const {
            return index_ >= static_cast<int>(handlersChain_.size());
        }

        /// 管理请求级作用域变量
        void set(const std::string& key, std::any value) {
            variables_[key] = std::move(value);
        }
        template<typename T>
        std::optional<T> get(const std::string& key) {
            // 查找
            if(const auto it = variables_.find(key); it != variables_.end()) {
                try {
                    // 尝试转换类型
                    return std::any_cast<T>(it->second);
                } catch (const std::bad_any_cast&) {
                    return std::nullopt;
                }
            }
            return std::nullopt;
        }


        /// 获取路径参数 /user/:id -> getParam("id")
        /// 不存在则返回空
        std::optional<std::string> pathVariable(const std::string& key) const {
            if(const auto it = params_.find(key); it != params_.end()) {
                return it->second;
            }
            return std::nullopt;
        }

        /// 获取查询参数 (适用于 ?key=value 格式)
        /// ?name=abc -> query("name") == abc
        std::optional<std::string> query(const std::string& key) const {
            const auto &queries = req_.getQueries();
            if(const auto it = queries.find(key); it != queries.end()) {
                return it->second;
            }
            return std::nullopt;
        }

        /// 获取表单参数 (适用于请求体的类型是 application/x-www-form-urlencoded)
        std::optional<std::string> form(const std::string& key) {
            const auto &form_data = req_.getFormData();
            if(const auto it = form_data.find(key); it != form_data.end()) {
                return it->second;
            }
            return std::nullopt;
        }

        /// 获取JSON参数 (适用于请求体的类型是 application/json)
        template <typename T>
        std::optional<T> bindJSON() const {
            try {
                return common::JsonUtil::fromJson<T>(req_.getBody());
            } catch (const std::exception&) {
                return std::nullopt;
            }
        }

        /// 根据Content-Type解析form-data或json参数
        template <typename T>
        std::optional<T> bind() const {
            const auto content_type = req_.getHeader("Content-Type");
            if (content_type == "application/x-www-form-urlencoded") {
                // 解析form-data
                try {
                    return common::JsonUtil::fromJson<T>(req_.getBody());
                } catch (const std::exception&) {
                    return std::nullopt;
                }
            } else if (content_type == "application/json") {
                // 解析JSON，调用bindJSON
                return bindJSON<T>();
            }
            return std::nullopt; // 不支持的Content-Type
        }

        /// 获取请求头
        /// 注意这里不能返回一个std::optional<std::string&>，因为引用是不可构造的
        std::optional<std::reference_wrapper<const std::string>> header(const std::string& key) const {
            const auto &headers = req_.headers();
            if(const auto it = headers.find(key); it != headers.end()) {
                return it->second;
            }
            return std::nullopt;
        }


        // 获取原始请求体
        HttpRequest& req() { return req_; }
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

        /// 重载
        template<typename T>
        void JSON(const HttpStatusCode code, T&& obj) {
            resp_.setStatusCode(code);
            resp_.setContentType("application/json");
            // 序列化
            try {
                resp_.setBody(common::JsonUtil::toJson<T>(std::forward<T>(obj)));
            } catch (const std::exception& e) {
                // 序列化失败则触发异常，交给全局异常处理器
                throw e;
            }
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

        /// 新增一个关键函数：需要在用户handler调用完毕后通过它来写回数据，实现了HttpServer的后半部分内容
        /// 它为业务线程提供了将响应写回IO线程的能力
        void flush() {
            if (conn_ && conn_->connected()) {
                Buffer buf;
                resp_.appendToBuffer(&buf);
                conn_->send(&buf); // 线程安全发送

                if (resp_.closeConnection()) {
                    conn_->shutdown();
                }
            } else {
                LOG_INFO("Connection doesn't exist");
            }
        }

        /// 重置Context的状态，主要是为了支持对象池
        void reset() {
            conn_.reset(); // 释放指针
            handlersChain_.clear();
            req_.reset(); // 重置请求
            resp_.reset(); // 重置响应体
            params_.clear(); // 清空参数
            variables_.clear(); // 清空作用域级别的变量
            index_ = -1; // 初始化状态
        }
        //
        // // 数据库相关操作
        // /// 注入数据库连接池
        // void setDbPool(const std::shared_ptr<IDbPool>& pool) {
        //     dbPool_ = pool;
        // }
        // /// 获取数据库连接池
        // std::shared_ptr<IDbPool> db() {
        //     return dbPool_;
        // }


    private:

        /// 中间件
        int index_ = -1; // 记录当前执行到哪个handler了

        std::vector<HandlerFunc> handlersChain_; // 存储实际要执行的handler链（它实际上就包括了中间件逻辑和实际的业务逻辑）

        /// 管理属于本次请求作用域内部的所有变量
        std::unordered_map<std::string, std::any> variables_;

        // 为了在执行业务逻辑时使用的不是IO线程，需要更改Context的声明周期（因为在执行业务逻辑时，IO线程已经执行完毕，之前HttpRequest和HttpResponse都是在IO栈上创建的，随着IO线程函数运行完毕，它们都会被释放）
        // const HttpRequest& req_;
        // HttpResponse* resp_;
        TcpConnectionPtr conn_;
        HttpRequest req_;
        HttpResponse resp_;
        std::unordered_map<std::string, std::string> params_; // 存储WebRouter解析出来的请求参数，包括路径参数、查询字符串、通配符字符串等
        //
        // // 数据库接口
        // /// 数据库连接池
        // std::shared_ptr<IDbPool> dbPool_;
    };
}

#endif //CONTEXT_H
