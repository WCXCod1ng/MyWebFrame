//
// Created by user on 2025/11/26.
//

#include "TcpConnection.h"

#include "Channel.h"
#include "EventLoop.h"
#include "Socket.h"
#include "log/Logger.h"
#include "coroutine/IoAwaiter.h"

namespace fleabane {
    // 静态成员初始化
    void defaultConnectionCallback(const TcpConnectionPtr& conn) {
        LOG_INFO("Connection {} is {}", conn->name(), (conn->connected() ? "UP" : "DOWN"));
    }

    void defaultMessageCallback(const TcpConnectionPtr&, Buffer* buf, TimeStamp) {
        buf->retrieveAll(); // 默认丢弃所有数据，防止堆积
    }

    TcpConnection::TcpConnection(EventLoop* loop,
                                 const std::string& nameArg,
                                 const int sockfd,
                                 const InetAddress& localAddr,
                                 const InetAddress& peerAddr,
                                 const double idleTimeoutSeconds)
        : ioLoop_(loop),
          name_(nameArg),
          state_(kConnecting),
          reading_(true),
          socket_(new Socket(sockfd)),
          channel_(new Channel(loop, sockfd)),
          localAddr_(localAddr),
          peerAddr_(peerAddr),
          highWaterMark_(64 * 1024 * 1024), // 64MB
          idleTimeoutSeconds_(idleTimeoutSeconds)
    {
        // 给 Channel 设置回调函数
        // 当 Poller 监听到事件后，会调用 Channel::handleEvent，进而调用这些函数
        channel_->setReadCallback(
            std::bind(&TcpConnection::handleChannelRead, this, std::placeholders::_1));
        channel_->setWriteCallback(
            std::bind(&TcpConnection::handleChannelWrite, this));
        channel_->setCloseCallback(
            std::bind(&TcpConnection::handleChannelClose, this));
        channel_->setErrorCallback(
            std::bind(&TcpConnection::handleChannelError, this));

        LOG_INFO("TcpConnection::ctor[{}] at fd={}", name_, sockfd);
        socket_->setKeepAlive(true); // 开启 TCP 保活
    }

    TcpConnection::~TcpConnection()
    {
        LOG_INFO("TcpConnection::dtor[{}] at fd={} state={}", name_, channel_->fd(), static_cast<int>(state_));
    }

    int TcpConnection::fd() const {
        return channel_->fd();
    }

    // [核心] 连接建立完成
    // 此函数由 TcpServer::newConnection 调用（在 MainLoop 中创建对象后，在 SubLoop 中执行此函数）
    // 它是连接生命的起点。虽然构造函数创建了对象，但直到这个函数被调用，连接才真正开始监听读事件（enableReading）
    void TcpConnection::connectEstablished()
    {
        ioLoop_->assertInLoopThread();

        LOG_INFO("连接建立 fd = {}", channel_->fd());

        assert(state_ == kConnecting);
        setState(kConnected);

        // 【重要】将 TcpConnection (shared_ptr) 绑定到 Channel 的 shared_ptr
        // 防止 Channel 在执行回调时，TcpConnection 对象已经被销毁
        channel_->tie(shared_from_this());

        // 向 Poller 注册 读 事件
        // 这是生命周期管理最关键的一步。如果不绑定，当连接断开时，TcpConnection 可能在 handleClose 执行一半时就被 TcpServer 从 map 中移除并析构，导致后续代码访问非法内存。
        channel_->enableReading();

        // 回调用户注册的 ConnectionCallback (告诉业务层：连接成功了)
        if (connectionCallback_) {
            connectionCallback_(shared_from_this());
        }

        // 连接建立时启动定时器
        extendLifetime();
    }

    /// 已连接Channel发现可读时实际上会调用该函数，执行实际的读事件
    ///
    /// 注意，它必须被执行在所分配的EventLoop所在的线程（所以需要在执行前assertInLoopThread()）
    void TcpConnection::handleChannelRead(TimeStamp receiveTime)
    {
        ioLoop_->assertInLoopThread();

        // 有数据来时说明对端还存活，应当刷新定时器
        extendLifetime();

        // // 1. 新增协程优先路径
        // if(coroutineCallback_) {
        //     LOG_DEBUG("协程模式：唤醒挂起的协程, fd = {}", channel_->fd());
        //     // 移动回调所有权，防止重复调用
        //     // 这一步很重要：将成员变量置空，表示当前没有协程在等待了
        //     auto resume_task = std::move(coroutineCallback_);
        //     coroutineCallback_ = nullptr;
        //     // 执行 resume，控制权交还给协程
        //     // 协程醒来后，会自己在 IoAwaiter::await_resume 中调用 readFd 读取数据
        //     resume_task();
        //     // 【关键】直接返回！
        //     // 千万不要执行下面的 inputBuffer_.readFd，否则数据会被这里读走，
        //     // 协程醒来后再读就读不到数据了（EAGAIN），导致逻辑错误。
        //     return;
        // }

        // 2. 【保留】传统回调路径 (Push模型)
        // 如果没有协程在等，说明是旧的 Reactor 模式，走原有流程
        int savedErrno = 0;

        // 从 Socket 读取数据读到 inputBuffer_
        // 利用 Buffer::readFd 的 readv 技术，自动处理读不够的情况
        ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);

        if (n > 0) {
            LOG_INFO("读事件触发的原因是：有数据可读 fd = {}", channel_->fd());
            // 读取成功，回调用户的 onMessage，表示收到消息
            if (messageCallback_) {
                messageCallback_(shared_from_this(), &inputBuffer_, receiveTime);
            }
        } else if (n == 0) {
            // 读到 0 字节，表示对端关闭了连接 (FIN)
            LOG_WARN("读事件触发的原因是：对端关闭，调用TcpConnection::handleChannelClose()");
            handleChannelClose();
        } else {
            // 出错
            errno = savedErrno;
            LOG_ERROR("连接出现意外错误，调用TcpConnection::handleChannelError()");
            handleChannelError();
        }
    }

    // [核心] 发送数据的对外接口
    void TcpConnection::send(const std::string& message)
    {
        send(message.data(), message.size());
    }

    void TcpConnection::send(const void *message, size_t len) {
        if (state_ == kConnected) {
            if (ioLoop_->isInLoopThread()) {
                // 在自己线程中调用，则直接执行
                sendInLoop(message, len);
            } else {
                // 如果在其他线程调用 send，需要转发给 IO 线程
                // 注意：这里必须拷贝 buf，因为它是引用，跨线程可能会失效
                // 优化点：C++11 move 语义可以减少拷贝，或者 sendInLoopString
                void (TcpConnection::*fp)(const void* data, size_t len) = &TcpConnection::sendInLoop;
                ioLoop_->runInLoop(std::bind(fp, this, message, len));
            }
        }
    }

    /// 这样的判断语句还是为了保证，IO写操作也在当初的那个线程中
    void TcpConnection::send(Buffer *buf) {
        if (state_ == kConnected) {
            if (ioLoop_->isInLoopThread()) {
                // 1. 如果在当前 IO 线程：
                // 直接把 Buffer 里的数据拿出来发送
                sendInLoop(buf->peek(), buf->readableBytes());
                // 发送完后，清空传入的 Buffer (通常语义是把数据“移交”给了连接)
                buf->retrieveAll();
            } else {
                // 2. 如果跨线程：
                // 必须把数据拷贝成 string (拥有所有权)，防止 Buffer 在原线程被销毁
                // 这里的 retrieveAllAsString 会做一次拷贝
                std::string msg = buf->retrieveAllAsString();
                ioLoop_->runInLoop(
                    std::bind(&TcpConnection::sendInLoopString, this, std::move(msg))); // 使用 move 减少开销
            }
        }
    }

    IoAwaiter TcpConnection::recv(Buffer *buf) {
        // 返回一个 Awaiter，编译器会生成协程状态机
        // 当用户 co_await 时，会调用 IoAwaiter 的 await_suspend
        return {this, buf, 0};
    }

    /// [核心] IO 线程内真正的发送逻辑
    ///
    /// 注意，也必须保证在所述的EventLoop所在的线程上被执行
    ///
    /// 在引入协程后，我们通常也不需要修改写入的逻辑：sendInLoop，因为业务逻辑的读与写通常具有非对称性
    /// - 读 (Read)：业务逻辑通常必须等待数据到达才能继续执行（比如必须读到 HTTP Header 才能解析）。因此我们需要 co_await conn->recv() 来挂起协程。
    /// - 写 (Write)：业务逻辑通常不需要等待数据真正发送到网卡。我们目前的 send 实现采用了 "应用层缓冲 (OutputBuffer)" 机制。当你调用 conn->send(buf) 时，数据只要拷贝到了 outputBuffer_，函数就返回了，协程就可以继续往下跑了。数据的实际发送由底层的 handleChannelWrite 在后台自动完成
    ///
    /// 除非想要实现：outputBuffer_满了（高水位），希望协程挂起，直到缓冲区有空位了再回复。这个时候需要在handleChannelWrite里面添加类似于handleChannelRead的唤醒逻辑
    void TcpConnection::sendInLoop(const void* data, size_t len)
    {
        ioLoop_->assertInLoopThread();

        LOG_INFO("向连接中发送数据, fd = {}", channel_->fd());

        ssize_t nWritten = 0;
        size_t remaining = len;
        bool faultError = false;

        // 之前已经调用过 shutdown，不能再发送了
        if (state_ == kDisconnected) {
            LOG_ERROR("disconnected, give up writing");
            return;
        }

        // 1. 尝试直接写 (Zero Copy 优化)
        // 条件：outputBuffer 没有积压数据（保证数据发送的有序性、outputBuffer中的数据是先发送的，应当先被write进socket） && Channel 没有关注写事件（关注了写事件会怎样？）
        if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
            LOG_DEBUG("触发zero copy优化，直接将待发送的数据写入socket");

            // fixme 直接写入时是否需要延长？
            extendLifetime();

            const char* ptr = static_cast<const char*>(data);
            // note ET模式下循环写，直到写完（或者写满）
            while(remaining > 0) {
                ssize_t n = ::write(channel_->fd(), ptr, remaining);
                if(n > 0) {
                    // 写入成功，更新数据
                    nWritten += n;
                    remaining -= n;
                    ptr += n;
                } else {
                    if(errno == EINTR) continue;
                    // ET模式写的结束（内核缓冲区已满），剩下的数据必须放到outputBuffer_中（由“2”负责）
                    if(errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    // 真正的错误
                    LOG_ERROR("直接写操作出错");
                    // nWritten = 0; // 注意，这里不能置为0，否则会导致数据重复发送

                    if (errno == EPIPE || errno == ECONNRESET) {
                        faultError = true;
                    }

                    // 发生不可恢复的错误时，必须break
                    break;
                }
            }

            // 如果没有出错的写完，则触发写完的回调
            if(!faultError && remaining == 0 && writeCompleteCallback_) {
                ioLoop_->queueInLoop(std::bind(writeCompleteCallback_, shared_from_this()));
            }
        }

        // 2. 如果没有写完（Socket 缓冲区满了）或者之前就有积压
        // 把剩余数据追加到 outputBuffer_，并开始关注 EPOLLOUT
        if (!faultError && remaining > 0) {
            size_t oldLen = outputBuffer_.readableBytes();

            // 高水位回调判断
            // 防止发送速度远快于对端接收速度，导致服务器内存耗尽
            if (oldLen + remaining >= highWaterMark_
                && oldLen < highWaterMark_
                && highWaterMarkCallback_)
            {
                ioLoop_->queueInLoop(std::bind(highWaterMarkCallback_, shared_from_this(), oldLen + remaining));
            }

            // 把剩余的部分追加到outputBuffer_中
            outputBuffer_.append(static_cast<const char*>(data) + nWritten, remaining);

            if (!channel_->isWriting()) {
                channel_->enableWriting(); // 注册写事件，等待 Epoll 通知 handleWrite，注意确保内部增加了EPOLLET
            }
        }
    }

    void TcpConnection::sendInLoopString(const std::string &message) {
        // throw std::runtime_error("未实现");
        sendInLoop(message.data(), message.size());
    }

    /// 已连接Channel发现可写时实际上会调用该函数，执行实际的写事件
    /// 从Buffer中的“可读区”读取数据并写入socket
    ///
    /// 注意，也必须保证在所述的EventLoop所在的线程上被执行
    void TcpConnection::handleChannelWrite()
    {
        ioLoop_->assertInLoopThread();

        LOG_INFO("有新数据要写, fd = {}", channel_->fd());

        if (channel_->isWriting()) { // 当前Channel关注了写事件

            // 有数据可写入时，可以刷新定时器
            extendLifetime();

            int saveErrno = 0;
            // 将 outputBuffer_ 中的数据写入 Socket
            ssize_t n = outputBuffer_.writeFd(channel_->fd(), &saveErrno);

            if (n >= 0) {
                // 检查数据是否发送完
                if (outputBuffer_.readableBytes() == 0) {

                    // 取消关注写事件，避免 busy loop，另外也是表明数据已经被发送完毕，如果现在要关闭，可以执行shutdownInLoop了
                    // 而且也会导致ET/LT模式空转
                    channel_->disableWriting();

                    // 触发 WriteComplete 回调
                    if (writeCompleteCallback_) {
                        ioLoop_->queueInLoop(std::bind(writeCompleteCallback_, shared_from_this()));
                    }

                    // 如果用户之前调用了 shutdown，但因为有数据没发完而被推迟了
                    // 现在发完了，执行真正的 shutdown
                    if (state_ == kDisconnecting) {
                        shutdownInLoop();
                    }
                } else {
                    // Buffer还没空，说明遇到EAGAIN了
                    // 在ET模式下继续保持关注EPOLLOUT
                    // note 只有使用了 EPOLLONESHOT 标志时，才需要在每次事件触发后重新注册（Rearm）。但在标准的 Reactor 模型（包括 Muduo 和你的实现）中，我们通常不使用 ONESHOT，所以不需要重复注册
                    LOG_INFO("ET write EAGAIN, wait for next EPOLLOUT");
                }
            } else {
                LOG_ERROR("TcpConnection::handleWrite");
            }
        } else {
            LOG_INFO("Connection fd={} is down, no more writing", channel_->fd());
        }
    }

    // [主动关闭]
    // 只是设置状态为正在关闭
    void TcpConnection::shutdown()
    {
        if (state_ == kConnected) {
            setState(kDisconnecting);
            ioLoop_->runInLoop(std::bind(&TcpConnection::shutdownInLoop, this));
        }
    }

    // 真正的关闭操作（socket_->shutdownWrite()）必须等到缓冲区数据清空后才能执行。这就是为什么在 handleWrite 里要检查 kDisconnecting 状态
    void TcpConnection::shutdownInLoop()
    {
        ioLoop_->assertInLoopThread();
        // 只有当 outputBuffer 数据全部发完，才能关闭写端
        // outputBuffer数据没有发送完，channel就还继续监听写事件
        if (!channel_->isWriting()) {
            socket_->shutdownWrite(); // 关闭写端，发送 FIN
        }
        // 如果 isWriting() 为真，说明还有数据没发完。
        // 我们只需设置 state_ = kDisconnecting。
        // 等 handleWrite 把数据发完后，会检查这个状态并执行 shutdownInLoop。
    }

    // [主动关闭] 强制关闭
    void TcpConnection::forceClose()
    {
        if (state_ == kConnected || state_ == kDisconnecting) {
            setState(kDisconnecting);
            ioLoop_->queueInLoop(std::bind(&TcpConnection::forceCloseInLoop, this));
        }
    }

    void TcpConnection::forceCloseInLoop()
    {
        ioLoop_->assertInLoopThread();
        if (state_ == kConnected || state_ == kDisconnecting) {
            handleChannelClose();
        }
    }

    void TcpConnection::extendLifetime() {
        // 如果定时器句柄是dangling的，说明这是首次调用extendLifetime（也即TcpConnection建立后通过connectEstablished所调用的），那么不需要操作，直接等待赋值即可
        // 如果不是dangling的，那么先取消旧的定时器
        if(ioLoop_ && !idleTimer_.dangling()) {
            ioLoop_->cancel(idleTimer_);
        }

        // 添加新的
        idleTimer_ = ioLoop_->runAfter(idleTimeoutSeconds_, std::bind(&TcpConnection::handleTimeout, shared_from_this()));
    }

    void TcpConnection::handleTimeout() {
        // 定时器到期，说明这段时间内没有 extendLife 被调用
        LOG_INFO("连接超时 fd={}", channel_->fd());

        // 强制关闭连接
        forceClose();
        // forceClose 会触发 handleClose -> removeConnection -> connectDestroyed
        // 最终完成清理
    }

    /// [被动关闭] 已连接Channel发现连接完全关闭时实际上会调用该函数，执行实际的关闭事件
    /// 是触发动作。当 read 返回 0 或 forceClose 被调用时执行。它的核心任务是通知 TcpServer：“我这个连接完了，请把我删掉”
    void TcpConnection::handleChannelClose()
    {
        ioLoop_->assertInLoopThread();
        LOG_INFO("连接完全关闭：fd={} state={}", channel_->fd(), (int)state_);
        setState(kDisconnected);

        // 1. 停止关注所有事件
        channel_->disableAll();

        // 2. 这里的 connectionCallback_ 依然是用户的 (通知用户连接断开了)
        TcpConnectionPtr guardThis(shared_from_this());
        if (connectionCallback_) {
            connectionCallback_(guardThis);
        }

        // 3. 这里的 closeCallback_ 是 TcpServer 注册的 (TcpServer::removeConnection)
        // 也就是通知 TcpServer 把这个 conn 从 map 中移除
        if (closeCallback_) {
            closeCallback_(guardThis);
        }
    }

    /// 已连接Channel发现连接出异常错误时实际上会调用该函数，执行实际的异常处理事件
    void TcpConnection::handleChannelError()
    {
        int optval;
        socklen_t optlen = sizeof optval;
        int err = 0;
        // 获取具体的 socket 错误码
        if (::getsockopt(channel_->fd(), SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0) {
            err = errno;
        } else {
            err = optval;
        }
        LOG_ERROR("TcpConnection::handleError name:{} - SO_ERROR:{}", name_, err);

        // 出错后通常连接也不可用了，直接处理关闭
        // handleClose();
    }

    // [最后一步] 连接彻底销毁
    // 由 TcpServer 从 map 中移除 conn 后调用
    // connectDestroyed: 是清理动作。TcpServer 在删除 shared_ptr 之前，最后调用一次这个函数，确保 Channel 从 Epoll 中移除
    void TcpConnection::connectDestroyed()
    {
        ioLoop_->assertInLoopThread();

        LOG_INFO("彻底销毁连接 fd = {}", channel_->fd());

        if (state_ == kConnected) {
            setState(kDisconnected);
            channel_->disableAll();
            if (connectionCallback_) {
                connectionCallback_(shared_from_this());
            }
        }

        // 连接关闭时应取消定时器
        // 如果不取消，TimerQueue中的回调函数（bind）将持有conn的shared_ptr，导致conn无法析构
        if(ioLoop_) {
            ioLoop_->cancel(idleTimer_);
        }

        // 从 Poller 中彻底移除 Channel
        channel_->remove();
    }
}