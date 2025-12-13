//
// Created by user on 2025/12/13.
//
#include "IoAwaiter.h"

#include <net/EventLoop.h>
#include <net/TcpConnection.h>

namespace fleabane {
    IoAwaiter::IoAwaiter(TcpConnection* conn, Buffer* buf, ssize_t bytesRead)
    : conn_(conn), buf_(buf), bytesRead_(bytesRead)
    {}

    /// 1. 检查是否真的需要挂起
    /// 返回 true: 不需要挂起，直接调用 await_resume
    /// 返回 false: 需要挂起，调用 await_suspend
    bool IoAwaiter::await_ready() const {
        // 优化：如果 Buffer 里已经有数据了，或者 socket 已经关闭了，就不需要挂起了
        // 这里只是简单判断，更严谨的做法可能需要尝试 peek 一下
        if (buf_->readableBytes() > 0) {
            return true;
        }
        return false;
    }

    /// 2. 挂起时的逻辑 (核心)
    /// @param handle 是当前协程的句柄，我们需要把它存起来，以便将来恢复
    void IoAwaiter::await_suspend(std::coroutine_handle<> handle) {
        // 将协程句柄注册给 TcpConnection
        // TcpConnection 需要新增一个方法 setCoroutineCallback 来保存这个回调
        conn_->setCoroutineCallback([handle, this]() {
            // 当数据到达时，Epoll 触发，执行这个 lambda

            // TODO: 这里是将来接入“业务线程池调度”的关键点
            // 目前暂时直接恢复（这意味着会在 IO 线程恢复）
            handle.resume();
        });

        // 开启读监听 (Epoll ADD EPOLLIN)
        // note 对于当前的实现，如下的代码没有其他副作用（因为我们在开始时刻就已经enableReading了），但是后续可能会在await_resume处调用disableReading
        // 将来引入disableReading的好处是：如果对端发送数据太快，muduo原本的逻辑的onMessage会持续读，直到Buffer OOM，必须在onMessage中判断并进行流量控制；
        // 但是在协程实现中，业务逻辑需要数据时才去co_await recv，如果对端发送数据太快，内核的TCP接收缓冲区就会满，内核会通知对端窗口为0停止发送
        conn_->getLoop()->runInLoop([this]() {
            conn_->enableReading();
        });
    }

    /// 3. 恢复时的逻辑
    /// 协程恢复执行后，co_await 表达式的返回值
    ssize_t IoAwaiter::await_resume() {
        // 协程醒来，说明数据到了 (或者一开始就有数据)
        // 执行真正的读取逻辑
        int saveErrno = 0;
        bytesRead_ = buf_->readFd(conn_->fd(), &saveErrno);

        if (bytesRead_ < 0) {
            // 错误处理，根据 errno 判断
            // 这里简单返回 -1
        }
        return bytesRead_;
    }
}
