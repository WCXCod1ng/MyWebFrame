#ifndef ASYNCMYSQLPOOL_H
#define ASYNCMYSQLPOOL_H
#include <deque>
#include <list>
#include <mysql.h>
#include <semaphore>
#include <base/ThreadPool.h>
#include <net/EventLoopThreadPool.h>

#include "DBInterfaces.h"

namespace sedum {
    class AsyncMySQLPool;

    /// MySQL连接
    class MySQLConnection : public IDbConnection, public std::enable_shared_from_this<MySQLConnection> {
    public:
        // 为了支持真正的异步，需要传入它所属的EventLoop，将来这个数据库连接上的就绪事件由指定的EventLoop负责
        MySQLConnection(MYSQL* conn, fleabane::EventLoop* loop) : mConn(conn), mLoop(loop) {
            // 一种做法是通过MariaDB来获取底层的fd
            mFd = conn->net.fd;

            // 创建一个Channel
            mChannel = std::make_unique<fleabane::Channel>(loop, mFd);
        }

        // 显式的清理函数，用于代替析构函数中的危险操作
        void forceClose() {
            // 确保在 IO 线程执行 Channel 清理
            mChannel->setReadCallback(nullptr);
            mChannel->setWriteCallback(nullptr);
            mChannel->setErrorCallback(nullptr);
            mLoop->runInLoop([self = shared_from_this()]() {
                if (self->mChannel) {
                    self->mChannel->disableAll();
                    self->mChannel->remove();
                }
                // mysql_close 的位置需要注意：
                // 如果你在 IO 线程 close，请确保没有其他线程还在用 mConn
                if (self->mConn) {
                    mysql_close(self->mConn);
                    self->mConn = nullptr;
                }
            });
        }

        ~MySQLConnection() override {
            if(mConn) {
                mysql_close(mConn);
                mConn = nullptr;
            }
        }

        Task<DBResult> query(std::string sql) override;

        Channel* channel() { return mChannel.get(); }

        MYSQL* raw() { return mConn; }
        fleabane::EventLoop* loop() { return mLoop; }
        int fd() const {return mFd;};
    private:

        MYSQL* mConn;
        fleabane::EventLoop* mLoop;
        int mFd;
        std::unique_ptr<fleabane::Channel> mChannel;
    };

    using MySQLConnectionPtr = std::shared_ptr<MySQLConnection>;

    /// 支持协程的异步MySQL连接池
    class AsyncMySQLPool final : public IDbPool, public std::enable_shared_from_this<AsyncMySQLPool> {
    public:
        /// 单例模式
        static AsyncMySQLPool& get_instance() {
            static AsyncMySQLPool instance;
            return instance;
        }

        /// 初始化连接池
        void init(
            const std::string &url,
            const std::string &user,
            const std::string &password,
            const std::string &dbname,
            int port,
            int max_conn,
            fleabane::ThreadPool* businessPool,
            const std::shared_ptr<EventLoopThreadPool>& ioLoopPool)
        {
            mUrl = url;
            mUser = user;
            mPassword = password;
            mDbname = dbname;
            mPort = port;
            mMaxConn = max_conn;

            assert(businessPool != nullptr);
            mBusinessPool = businessPool;
            mIoLoopPool = ioLoopPool; // 保存IO线程池

            // 在创建之前，加锁以保证线程安全
            std::lock_guard<std::mutex> lock(mMutex);

            // 循环创建max_conn个数量的数据库连接
            for(int i = 0; i < mMaxConn; ++i) {
                MYSQL * init_conn = mysql_init(nullptr);

                // 检查是否初始化成功
                if(!init_conn) {
                    throw std::runtime_error("MySQL Error: mysql_init() failed.");
                }

                // 尝试建立真正的物理连接
                MYSQL* connect_result = mysql_real_connect(init_conn, mUrl.c_str(), mUser.c_str(), mPassword.c_str(), mDbname.c_str(), mPort, nullptr, 0);

                // 检查连接是否成功
                if(!connect_result) {
                    // 如果连接失败，获取具体的错误信息，然后抛出异常
                    // 注意：根据MySQL C API，如果 mysql_real_connect 失败，它不会自动释放
                    // 由 mysql_init 创建的句柄，我们需要手动关闭它以防内存泄漏。
                    // 但为了获取错误信息，我们需要先从这个句柄中读取。
                    const std::string error_msg = mysql_error(init_conn);
                    mysql_close(init_conn); // 清理失败的句柄
                    throw std::runtime_error("MySQL Error: mysql_real_connect() failed: " + error_msg);
                }

                // 关键，为每个连接分配一个Muduo EventLoop
                auto loop = mIoLoopPool->getNextLoop();

                // 创建成功则加入队列
                mConns.push_back(new MySQLConnection(init_conn, loop));
            }

            // 更新信号量，通知有m_max_conn个资源可用
            if(mMaxConn > 0) {
                mReserve.release(mMaxConn);
            }
        }

        /// 销毁连接池
        void destroy_pool() {
            // 上锁，确保在销毁的过程中没有其他线程在操作队列
            std::lock_guard<std::mutex> lock(mMutex);

            // 循环遍历并关闭池中所有的空闲连接
            while(!mConns.empty()) {
                auto conn = mConns.front();
                mConns.pop_front();

                // 显式关闭
                conn->forceClose();
            }
        } // m_reserve的析构函数会自动清理其资源，因此无需我们手动操作

        /// 获取连接，需要使用异步方式
        Task<std::shared_ptr<IDbConnection>> getConnectionAsync() override {
            // 返回 Awaiter，触发 co_await 机制
            // 这里还需要使用co_return，因为我们有返回值，这个返回值就是await_resume()的结果（一个连接），它会通过return_value()传递给Task，又通过Task.await_resume()返回出去
            MySQLConnection* raw_conn = co_await ConnectionAwaiter{*this};

            // 封装成shared_ptr
            std::shared_ptr<IDbConnection> conn_ptr(
                raw_conn,
                [this](IDbConnection *p) {
                    // 自定义删除器
                    // 下转型
                    auto mysql_conn = dynamic_cast<MySQLConnection*>(p);
                    if(mysql_conn == nullptr) {
                        return;
                    }

                    // 清空监听的事件
                    auto loop = mysql_conn->loop();
                    loop->runInLoop([mysql_conn]() {
                        auto channel = mysql_conn->channel();
                        channel->disableAll();
                    });

                    // 归还给连接池
                    this->releaseConnection(mysql_conn);
                }
            );

            // 返回出去
            co_return conn_ptr;
        }


    private:

        // 构造函数和析构函数设置为私有，放置被外界创建和销毁
        // 这里设置为20，但是实际上要更大（CPU核数*2+磁盘IO等待系数，或者直接等于最大连接数）
        AsyncMySQLPool() : mReserve(0) {}
        ~AsyncMySQLPool() override {
            destroy_pool();
        }


        /// 获取业务线程池
        fleabane::ThreadPool* getBusinessPool() {
            return mBusinessPool;
        }

        // ==========================================
        // 核心改造点：自定义 Awaiter
        // ==========================================
        struct ConnectionAwaiter {
            AsyncMySQLPool& mPool;
            MySQLConnection* resConn = nullptr; // 用于传递结果

            // 1. 是否需要挂起？
            bool await_ready() {
                std::lock_guard<std::mutex> lock(mPool.mMutex);
                // 如果有空闲连接，就不挂起，直接拿走
                if (!mPool.mConns.empty()) {
                    resConn = mPool.mConns.front();
                    mPool.mConns.pop_front();
                    return true; // true = 不挂起
                }
                return false; // false = 挂起，进入 await_suspend
            }

            // 2. 挂起时的逻辑
            void await_suspend(std::coroutine_handle<> cur_handle) {
                std::lock_guard<std::mutex> lock(mPool.mMutex);
                // 再次检查（双重检查），防止挂起瞬间刚好有连接归还
                if (!mPool.mConns.empty()) {
                    resConn = mPool.mConns.front();
                    mPool.mConns.pop_front();
                    cur_handle.resume(); // 立即恢复
                    return;
                }

                // 真的没连接了，把句柄和"我"（Awaiter本身）存入等待队列
                // 为什么存 Awaiter 指针？因为我们需要把连接塞回 conn_ 成员变量里传给协程
                mPool.mWaiters.push_back({cur_handle, this});
            }

            // 3. 恢复后拿结果
            MySQLConnection* await_resume() {
                return resConn;
            }
        };

        /// 归还连接
        /// 不由业务逻辑线程调用，而是由智能指针管理
        /// 此时conn对应的Channel已经是干净的状态，可以直接交付了
        void releaseConnection(MySQLConnection* conn) {
            Waiter waiter = {nullptr, nullptr}; // 临时保存等待者

            {
                // 1. 最小化锁的范围，只保护队列操作
                std::lock_guard<std::mutex> lock(mMutex);

                if (!mWaiters.empty()) {
                    waiter = mWaiters.front();
                    mWaiters.pop_front();
                } else {
                    // 没人排队，归还连接
                    mConns.push_back(conn);
                }
            } // 锁在这里释放

            // 2. 在锁外处理恢复逻辑
            if (waiter.handle != nullptr && waiter.awaiter != nullptr) {
                // 将连接传递给等待者
                waiter.awaiter->resConn = conn;

                // 【关键修复】打断递归！
                // 不要直接 waiter.handle.resume();
                // 而是扔回业务线程池调度
                mBusinessPool->enqueue([h = waiter.handle]() {
                    h.resume();
                });
            }
        }


        // 数据库信息
        std::string mUrl; // 数据库地址
        std::string mUser; // 用户名
        std::string mPassword; // 密码
        std::string mDbname; // 数据库名称
        int mPort = 0; // 端口号

        // 连接池信息
        int mMaxConn = 0; // 最大连接数量
        int mCurConn = 0; // 当前已使用的连接数
        int mFreeConn = 0; // 当前空闲的连接数

        fleabane::ThreadPool* mBusinessPool = nullptr;
        std::shared_ptr<EventLoopThreadPool> mIoLoopPool;

        std::mutex mMutex; // 互斥锁
        std::counting_semaphore<> mReserve; // 信号量默认被初始化为0，将来会在init函数中被初始化为最大连接个数
        std::list<MySQLConnection*> mConns; // 由于一个连接需要包含与之关联的fd、loop等信息，所以这里封装了一层

        // 支持协程的异步操作
        struct Waiter {
            std::coroutine_handle<> handle;
            ConnectionAwaiter* awaiter;
        };
        std::deque<Waiter> mWaiters; // 等待连接的协程队列

        friend struct ConnectionAwaiter;
        friend class MySQLConnection;

    };

    /// 清空channel上的监听事件
    inline void clear_channel(Channel* channel) {
        channel->setReadCallback(nullptr);
        channel->setWriteCallback(nullptr);
        channel->setErrorCallback(nullptr);
        auto loop =channel->ownerLoop();
        loop->runInLoop([channel]() {
            channel->disableAll();
        });
    }

    struct QueryAwaiter {
        // SQL语句
        std::string mSql;
        // 数据库连接
        std::shared_ptr<MySQLConnection> mConn;

        // 业务线程池的指针，用于将控制流传递给业务协程
        fleabane::ThreadPool* mBusinessPool;

        // 结果和异常
        MYSQL_RES* mRes = nullptr;
        DBResult mResult;
        std::exception_ptr mEx;

        // 状态机控制，表明当前处于哪个阶段，初始时刻是QUERY阶段
        enum class State {
            QUERY, // 当前连接正在执行QUERY阶段（等待数据库查询）
            STORE_RESULT, // 当前连接正在执行处理结果阶段
            FETCH_ROW,
            DONE // 当前连接已经处理完，可以将结果交给业务协程
        } mStatus = State::QUERY;


        QueryAwaiter(std::shared_ptr<MySQLConnection> conn, std::string sql, fleabane::ThreadPool* pool)
        : mConn(std::move(conn)), mSql(std::move(sql)), mBusinessPool(pool){}

        bool await_ready() { return false; } // 总是挂起

        void await_suspend(std::coroutine_handle<> h) {
            // 尝试直接驱动状态机：fast path
            drive_state_machine(h);

            // 如果状态机内部由于遇到了NOT_READY而导致状态机没有进入DONE状态，并且也没有发生错误，那么就需要向Muduo中注册该事件
            if(mStatus != State::DONE && !mEx) {
                mConn->loop()->runInLoop([this, h]() {
                    setup_io_handler(h);
                });
            }
        }

        DBResult await_resume() {
            if(mEx) {
                std::rethrow_exception(mEx);
            }
            return mResult;
        }



        /// 核心：在 IO 线程中驱动状态机
        /// slow path
        void setup_io_handler(std::coroutine_handle<> h) {
            auto channel = mConn->channel();

            // 处理“读”就绪
            // MySQL 8.0 官方文档指出：在 NOT_READY 状态下，
            // 应用程序应该根据 Socket 是否可读/可写来决定何时重试。
            // 通常查询操作是监听“可读”事件来获取结果。
            channel->setReadCallback([this, h](TimeStamp ts) {
                // 在 ET 模式下，当读事件触发，我们传给 MariaDB MYSQL_WAIT_READ
                this->drive_state_machine(h);
            });

            // 处理“写”就绪
            channel->setWriteCallback([this, h]() {
                // 在 ET 模式下，当写事件触发，我们传给 MariaDB MYSQL_WAIT_WRITE
                this->drive_state_machine(h);
            });

            // 错误处理
            channel->setErrorCallback([this, h]() {
                handle_error(h, mConn->raw());
            });

            // 开启监听
            auto loop = channel->ownerLoop();
            loop->runInLoop([channel]() {
                channel->enableReading();
                channel->enableWriting();
            });
        }

        // 驱动状态机的核心函数
        void drive_state_machine(std::coroutine_handle<> h) {
            if(mStatus == State::DONE) return; // 防止出现一个事件处理完，但是还没来得及clear_channel，而此时同一个Channel上的另一个事件也被触发了
            net_async_status async_status; // 异步API的执行状态
            MYSQL * mysql_ptr = mConn->raw();

            while(true) {
                switch (mStatus) {
                    case State::QUERY :
                    // 重复调用 query 函数，直到它不再返回 NOT_READY
                    async_status = mysql_real_query_nonblocking(mysql_ptr, mSql.c_str(), mSql.length());

                    // 没有就绪则等待下一次事件
                    if(async_status == NET_ASYNC_NOT_READY) return;

                    // 报错则处理错误
                    if(async_status == NET_ASYNC_ERROR) {
                        handle_error(h, mConn->raw());
                        return;
                    }

                    // 到此说明query完成，下一步，获取结果集
                    mStatus = State::STORE_RESULT;
                    break; // 继续continue，接下来会立即执行STORE_RESULT对应的处理函数

                    case State::STORE_RESULT :
                        // Store Result 完成，处理元数据并决定下一步
                        if(!prepare_result_metadata(h, mysql_ptr)) return; // 不需要状态机循环了
                        // if (mStatus == State::DONE) return;
                        break; // 继续 while 循环，进入 FETCH_ROW

                    case State::FETCH_ROW:
                        if(!handle_fetch_row(h)) return; // 不需要状态机循环了
                        break; // 否则继续 while 循环，进入 DONE

                    default:
                        finish_and_resume(h);
                        return;
                }
            }
        }

        /// 处理错误
        /// 恢复协程
        void handle_error(std::coroutine_handle<> h, MYSQL * mysql) {
            // 我们需要立即处理，虽然修改epoll无法立即完成，但是可以提前将回调置空
            clear_channel(mConn->channel());

            mEx = std::make_exception_ptr(std::runtime_error(mysql_error(mysql))); // 抛出异常
            mStatus = State::DONE; // 也将结果置为DONE，防止后续连接处理
            // 提交给业务线程池：恢复业务协程执行
            mBusinessPool->enqueue([h]() { h.resume(); });
        }


        /// 解析结果集的元数据信息
        /// 如果有结果集，则同步等待元数据，并且将状态转移到FETCH_ROW
        /// 如果没有结果集（INSERT、UPDATE），那么不会经过FETCH_ROW，而是直接执行finish_and_resume，将状态转移到DONE
        /// 同样返回外层是否需要继续
        bool prepare_result_metadata(std::coroutine_handle<> h, MYSQL* mysql_ptr) {
            if(mStatus != State::STORE_RESULT) return false;
            net_async_status async_status = mysql_store_result_nonblocking(mysql_ptr, &mRes);
            // 没有就绪则等待下一次事件
            if(async_status == NET_ASYNC_NOT_READY) return false;

            // 报错则处理错误
            if(async_status == NET_ASYNC_ERROR) {
                handle_error(h, mConn->raw());
                return false;
            }

            if (mRes) {
                // --- 情况 A: 有结果集 (SELECT, SHOW 等) ---
                unsigned int num_fields = mysql_num_fields(mRes);
                my_ulonglong num_rows = mysql_num_rows(mRes); // 注意：store_result 后行数已知

                // 存储列信息
                MYSQL_FIELD* fields = mysql_fetch_fields(mRes);
                mResult.columns.reserve(num_fields);
                for (unsigned int i = 0; i < num_fields; i++) {
                    mResult.columns.emplace_back(fields[i].name);
                }
                // 预分配行空间
                mResult.rows.reserve(num_rows);

                // 状态流转：进入行获取阶段
                mStatus = State::FETCH_ROW;
                // 注意：此时不要 disableAll Channel，因为 fetch_row 还需要网络
            } else {
                // --- 情况 B: 无结果集 (INSERT, UPDATE) ---
                if (mysql_field_count(mConn->raw()) == 0) {
                    mResult.affectedRows = mysql_affected_rows(mConn->raw());
                    mResult.insertId = mysql_insert_id(mConn->raw());
                } else {
                    handle_error(h, mConn->raw()); // 应该是 SELECT 但返回了 NULL
                    return false;
                }
                // 不需要 Fetch，直接完成
                // finish_and_resume(h);
                mStatus = State::DONE; // 我们这里只修改状态，让外层的drive_state_machine决定什么动作
            }

            return true;
        }

        /// 释放MySQL的结果集资源
        /// 同步清理Channel
        /// 恢复业务协程
        void finish_and_resume(std::coroutine_handle<> h) {
            if(mStatus != State::DONE) return;
            // 1. 释放 MySQL 结果集资源
            if (mRes) {
                mysql_free_result(mRes);
                mRes = nullptr;
            }

            // 2. 清理 Muduo Channel (同步清理)
            // 确保不会再触发任何回调
            auto channel = mConn->channel();
            clear_channel(channel);

            // 3. 恢复业务协程
            mBusinessPool->enqueue([h]() { h.resume(); });
        }

        /// 处理mStatus == FETCH_ROW的情况：获取每一行
        /// 返回是否需要继续状态机循环
        bool handle_fetch_row(std::coroutine_handle<> h) {
            if(mStatus != State::FETCH_ROW) return false;
            MYSQL_ROW row;
            net_async_status async_status;

            while (true) {
                async_status = mysql_fetch_row_nonblocking(mRes, &row);

                if (async_status == NET_ASYNC_NOT_READY) {
                    // 本地缓冲区空了，需要等待网络数据。
                    // 此时 Channel 依然开启 Read 事件，直接返回，等待下一次回调。
                    return false;
                }

                if (async_status == NET_ASYNC_ERROR) {
                    handle_error(h, mConn->raw());
                    return false;
                }

                // status == NET_ASYNC_COMPLETE
                if (row) {
                    // --- 解析单行数据 ---
                    unsigned int num_fields = mysql_num_fields(mRes);
                    unsigned long* lengths = mysql_fetch_lengths(mRes);
                    std::vector<std::string> row_data;
                    row_data.reserve(num_fields);

                    for (unsigned int i = 0; i < num_fields; i++) {
                        if (row[i]) {
                            row_data.emplace_back(row[i], lengths[i]);
                        } else {
                            row_data.emplace_back("");
                        }
                    }
                    mResult.rows.push_back(std::move(row_data));

                    // 【循环继续】：继续尝试获取下一行
                } else {
                    // row == nullptr，表示所有行获取完毕，将状态转移到DONE
                    mStatus = State::DONE;
                    // finish_and_resume(h);
                    return true;
                }
            }
        }
    };


    // 查询的实现
    inline Task<DBResult> MySQLConnection::query(const std::string sql) {
        // 获取全局的MySQL线程池
        auto &pool = AsyncMySQLPool::get_instance();
        // 通过QueryAwaiter异步等待查询结果
        co_return co_await QueryAwaiter{shared_from_this(), sql, pool.getBusinessPool()};
    }

}

#endif //ASYNCMYSQLPOOL_H
