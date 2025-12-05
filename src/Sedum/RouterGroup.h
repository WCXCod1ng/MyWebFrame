//
// Created by user on 2025/12/4.
//

#ifndef ROUTERGROUP_H
#define ROUTERGROUP_H
#include <string>
#include <WebRouter.h>

namespace sedum {
    // 前向声明
    class WebFrame;

    /// 定义路由组，用于实现分组路由（类似于Gin）
    class RouterGroup {
    public:
        using HandlerFunc = common::HandlerFunc;

        RouterGroup(std::string prefix, WebRouter& router)
            : prefix_(std::move(prefix)), router_(router) {}

        // --- 核心功能 1: 创建子路由组 ---
        // 例如 group("/v1") -> 新的前缀为 "/api/v1"
        RouterGroup group(const std::string& relativePath);

        // --- 核心功能 2: 注册中间件 ---
        // 该组下的所有路由都会应用这些中间件
        void use(HandlerFunc middleware);

        // --- 核心功能 3: 注册路由 ---
        void GET(const std::string &relativePath, HandlerFunc handler);
        void POST(const std::string& relativePath, HandlerFunc handler);
        void PUT(const std::string& relativePath, HandlerFunc handler);
        void DELETE(const std::string& relativePath, HandlerFunc handler);
        void HEAD(const std::string& relativePath, HandlerFunc handler);
        // ... 其他方法 ...

        // 内部方法：获取该组的所有中间件
        [[nodiscard]] const std::vector<HandlerFunc>& getMiddlewares() const { return middlewares_; }

    private:
        // 注册路由的底层实现
        void addRoute(Method method, const std::string& relativePath, HandlerFunc handler);

        // 合并路径：当前组前缀 + 相对路径
        [[nodiscard]] std::string combinePath(const std::string& relativePath) const;

        std::string prefix_;   // 当前组的绝对前缀 (e.g. "/api/v1")
        WebRouter &router_; // 底层路由器实例的引用，用于实际的路由注册和查找
        // WebFrame* engine_;     // 指向 WebFrame (Engine) 实例，用于操作底层的 WebRouter，这里选择指针的目的是为了使得 RouterGroup 可拷贝，而且我们保证 engine_ 指向的实例在 RouterGroup 生命周期内始终有效。RouterGroup要么作为WebFrame的成员存在，要么被WebFrame创建出来，二者的生命周期都不会超过WebFrame本身

        // 当前组专属的中间件链
        std::vector<HandlerFunc> middlewares_;
    };
}

#endif //ROUTERGROUP_H
