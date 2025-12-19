//
// Created by user on 2025/12/4.
//

#ifndef DEFINE_H
#define DEFINE_H
#include "Context.h"
#include "coroutine/Coroutine.h"

/// ⚠ 正确做法：前置声明 sedum::Context
/// 这样的前置声明不会引入循环依赖问题，而且后续使用HandlerFunc不会认为Context是common命名空间下的类型，而是sedum命名空间下的类型
namespace sedum { class Context; }

namespace common {
    /// 定义业务处理函数的签名
    /// 类似于 Spring 中的 Controller 方法
    using HandlerFunc = std::function<sedum::Task<void>(const std::shared_ptr<sedum::Context>&)>;
    /// 定义异常处理器的签名
    using ExceptionHandler = std::function<void(const std::shared_ptr<sedum::Context>&, const std::exception&)>;
    /// 定义handlers_chain类型：中间件+业务，用以支持像Gin那样的洋葱模型
    using HandlersChain = std::vector<HandlerFunc>;
}

#endif //DEFINE_H
