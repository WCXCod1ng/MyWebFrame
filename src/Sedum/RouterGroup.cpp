//
// Created by user on 2025/12/4.
//
#include "RouterGroup.h"
#include "WebFrame.h"

namespace sedum {
    RouterGroup RouterGroup::group(const std::string& relativePath) {
        // 递归创建子组：新前缀 = 当前前缀 + 相对路径
        // 将来WebFrame会持有一个根group，一切的其他group都会由它的group函数来创建
        const std::string newPrefix = combinePath(relativePath);
        RouterGroup newGroup(newPrefix, engine_);

        // 关键点：子组继承父组的中间件
        // Gin 的逻辑是 copy 父组的中间件到子组
        newGroup.middlewares_ = this->middlewares_;

        return newGroup;
    }

    void RouterGroup::use(HandlerFunc middleware) {
        middlewares_.push_back(std::move(middleware));
    }

    void RouterGroup::GET(const std::string& relativePath, HandlerFunc handler) {
        addRoute(Method::kGet, relativePath, std::move(handler));
    }
    void RouterGroup::POST(const std::string &relativePath, HandlerFunc handler) {
        addRoute(Method::kPost, relativePath, std::move(handler));
    }
    void RouterGroup::PUT(const std::string &relativePath, HandlerFunc handler) {
        addRoute(Method::kPut, relativePath, std::move(handler));
    }
    void RouterGroup::DELETE(const std::string &relativePath, HandlerFunc handler) {
        addRoute(Method::kDelete, relativePath, std::move(handler));
    }
    void RouterGroup::HEAD(const std::string &relativePath, HandlerFunc handler) {
        addRoute(Method::kHead, relativePath, std::move(handler));
    }

    void RouterGroup::addRoute(Method method, const std::string& relativePath, HandlerFunc handler) {
        // 1. 计算完整路径
        const std::string finalPath = combinePath(relativePath);

        // 2. 组装完整的处理链 (HandlersChain)
        // 顺序：[组中间件..., 业务Handler]

        std::vector<HandlerFunc> chain = middlewares_; // 拷贝当前组的中间件
        chain.push_back(std::move(handler));           // 最后是业务逻辑

        // 3. 注册到底层 WebRouter
        engine_->router().add_route(finalPath, method, std::move(chain));
    }

    std::string RouterGroup::combinePath(const std::string& relativePath) const {
        if (prefix_.empty()) return relativePath;
        if (relativePath.empty()) return prefix_;

        // 处理斜杠拼接 (简化版)
        if (prefix_.back() == '/' && relativePath.front() == '/') {
            return prefix_ + relativePath.substr(1);
        }
        if (prefix_.back() != '/' && relativePath.front() != '/') {
            return prefix_ + "/" + relativePath;
        }
        return prefix_ + relativePath;
    }


}