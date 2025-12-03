//
// Created by user on 2025/11/30.
//


#include <jwt-cpp/traits/kazuho-picojson/defaults.h>

#include "../../Fleabane/http/HttpResponse.h"
#include "../../Fleabane/log/Logger.h"
#include "../../Fleabane/net/InetAddress.h"
#include "../../Sedum/Context.h"
#include "../../Sedum/WebFrame.h"

#include "../../Fleabane/utils/ConcurrentQueue.h"

using namespace fleabane;
using namespace sedum;

const std::string secret = "my-secret";

// 记录执行时间的中间件
void execution_time_middleware(Context& ctx) {
    // 使用单调时钟记录处理本次请求的时间
    std::chrono::time_point<std::chrono::steady_clock> start = std::chrono::steady_clock::now();
    LOG_INFO("请求到来");
    try {
        ctx.next();
    } catch (std::exception& e) {
        std::chrono::time_point<std::chrono::steady_clock> end = std::chrono::steady_clock::now();
        LOG_INFO("请求发生异常，经过 {}ms", std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
        throw std::runtime_error(std::string("触发异常，异常信息为") + e.what());
    }
    std::chrono::time_point<std::chrono::steady_clock> end = std::chrono::steady_clock::now();
    LOG_INFO("请求结束，经过 {}ms", std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
}

// 校验用户身份的中间件
void auth_middleware(Context& ctx) {
    // 这里固定写死，实际需要引入配置文件（类）
    if(ctx.req().url() != "/register") {

        // 校验是否携带了Header "Authorization"
        if(auto token = ctx.header("Authorization"); token) {
            try {
                auto decoded = jwt::decode(token->get());
                jwt::verify()
                    .allow_algorithm(jwt::algorithm::hs256{secret})
                    .with_issuer("auth_server")
                    .verify(decoded);
                // 到此校验成功，提取user_id
                std::string user_id = decoded.get_payload_claim("user_id").as_string();
                LOG_INFO("用户{}登录成功", user_id);
                // 设置到ctx中
                ctx.set("user_id", user_id);
                // important，要执行next用以传递到后续的操作中
                ctx.next();
            } catch (const std::exception& e) {
                // 校验失败
                ctx.STR(fleabane::HttpStatusCode::k403Forbidden, "wrong authorization");
            }
        } else {
            // 403 forbidden
            ctx.STR(fleabane::HttpStatusCode::k403Forbidden, "without authorization");
        }
    } else {
        // register接口则直接next
        ctx.next();
    }
}


int main() {
    // 开启日志，级别为 INFO
    Logger::Config log_config;
    log_config.log_folder = "/root/code/cpp/MyTinyWebServer/out/log"; // 日志文件存储的路径
    log_config.max_queue_size = 1024;     // 开启异步日志
    LogLevel default_level = LogLevel::INFO; // 设置默认日志等级
    // note 在debug的时候默认设置为true，方便debuug
    log_config.is_override = true;
    log_config.enable_console_sink = true;
    log_config.flush_interval_seconds = 0; // 同步刷新
    Logger::get_instance().init(log_config);

    const InetAddress addr(9006);
    WebFrame app(addr, "SmartWeb");

    app.use(execution_time_middleware);
    app.use(auth_middleware);

    // 模拟用户注册，这里简单编写为直接返回一个签名后的jwt
    app.POST("/register", [](Context& ctx) {
        auto token = jwt::create()
            .set_issuer("auth_server")
            .set_type("JWS")
            .set_issued_at(std::chrono::system_clock::now())
            .set_expires_at(std::chrono::system_clock::now() + std::chrono::seconds(3600)) // 设置超时时间为1小时
            .set_payload_claim("user_id", jwt::claim(std::string("12345")))
            .sign(jwt::algorithm::hs256{secret});
        ctx.STR(fleabane::HttpStatusCode::k200Ok, token);
    });

    // 测试请求级作用域变量传递
    app.GET("/user", [](Context& ctx) {
        auto user_id = ctx.get<std::string>("user_id");
        if(user_id) {
            ctx.STR(HttpStatusCode::k200Ok, "Hello:" + *user_id);
        } else {
            throw std::runtime_error("状态错误，找不到user_id");
        }
    });

    // 注册GET方法
    app.GET("/user/:id", [](sedum::Context& ctx) {
        if(const auto user_id = ctx.pathVariable("id")) {

            LOG_INFO("GET方法被执行到");

            ctx.JSON(HttpStatusCode::k200Ok, "{\"id\": " + *user_id + "}");
        } else {
            throw std::runtime_error("异常，没有匹配到任何内容");
        }
    });

    // 测试异常处理
    app.POST("/panic", [](Context& ctx) {
       throw std::runtime_error("故意抛出一个异常");
    });

    // 测试查询参数
    app.GET("/user/query", [](Context& ctx) {
        if (const auto name = ctx.query("name")) {
            ctx.STR(HttpStatusCode::k200Ok, "hello " + *name);
        }
    });

    // 自定义全局异常处理 (覆盖默认行为)
    app.setExceptionHandler([](Context& ctx, const std::exception& e) {
        // 比如记录到日志文件
        // LOG_ERROR("Global Exception: {}", e.what());
        // 返回友好的 JSON 错误信息
        ctx.JSON(HttpStatusCode::k500InternalServerError, "{\"error\": \"系统繁忙，请稍后再试\"}");
    });

    // 自定义 404 页面
    app.setNotFoundHandler([](Context& ctx) {
        ctx.resp().setStatusCode(HttpStatusCode::k404NotFound);
        ctx.resp().setBody("<h1>My Custom 404 Page</h1>");
    });

    // 自定义 405 页面
    app.setMethodNotAllowedHandler([](Context& ctx) {
        ctx.resp().setStatusCode(HttpStatusCode::k405MethodNotAllowed);
        ctx.resp().setBody("<h1>My Custom 405 Page</h1>");
    });


    app.start();

    return 0;
}
