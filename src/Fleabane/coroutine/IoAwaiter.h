//
// Created by user on 2025/12/13.
//

#ifndef IOAWAITER_H
#define IOAWAITER_H
#include <coroutine>
#include "base/Buffer.h"

namespace fleabane {
    class TcpConnection;
    class Buffer;

    /// 当在协程中写 ssize_t n =  co_await IoAwaiter(conn, buf) 时，编译器需要知道如何挂起、如何恢复。这就要靠 Awaiter（等待体）
    /// 1. await_ready()：
    ///     - 编译器问：“现在数据准备好了吗？”
    ///     - 如果 Buffer 里已经有上次没读完的数据，返回 true。编译器跳过挂起，直接去调 await_resume()。这避免了不必要的 Epoll 交互，极大提升性能。
    ///     - 如果没数据，返回 false，进入挂起流程。
    /// 2. await_suspend(h)：
    ///     - 上下文切换：此时协程暂停，函数栈被保存到堆上。
    ///     - 注册回调：我们把 h（协程的遥控器）封装进一个 Lambda，塞给 TcpConnection。告诉它：“一旦 socket 有动静，别调原来的 onMessage 了，调这个 Lambda。”
    ///     - 开启监听：调用 enableReading，告诉 Epoll 关注读事件。
    ///     - 控制权交还：await_suspend 返回 void，控制权交还给调用者（通常是 EventLoop 的循环）。EventLoop 继续去处理其他 socket。
    /// 3. Epoll 唤醒：
    ///     - 数据到了 -> EventLoop::loop -> Channel::handleRead -> 执行刚才注册的 Lambda -> h.resume()。
    /// 4. await_resume()：
    ///     - 协程被 resume，就像从睡梦中惊醒。
    ///     - 此时代码回到了 IoAwaiter 内部。
    ///     - 调用 readFd 真正把数据从内核读到 Buffer。
    ///     - return bytesRead_：这个返回值赋给了你代码里的变量 n。
    class IoAwaiter {
    public:
        IoAwaiter(TcpConnection *conn, Buffer *buf, ssize_t bytesRead);

        bool await_ready() const;

        void await_suspend(std::coroutine_handle<> handle);

        ssize_t await_resume();

    private:

        TcpConnection* conn_;
        Buffer* buf_;
        ssize_t bytesRead_;
    };
}



#endif //IOAWAITER_H
