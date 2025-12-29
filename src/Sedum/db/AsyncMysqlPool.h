#ifndef ASYNCMYSQLPOOL_H
#define ASYNCMYSQLPOOL_H
#include <deque>
#include <list>
#include <mysql.h>
#include <semaphore>
#include <base/ThreadPool.h>

#include "DBInterfaces.h"

namespace sedum {
    class AsyncMySQLPool;

    /// MySQL连接
    class MySQLConnection : public IDbConnection {
    public:
        MySQLConnection(MYSQL* conn) : conn_(conn) {}

        // 后面我们会详细讲这个 query 怎么实现
        Task<DBResult> query(std::string sql) override;

        MYSQL* raw() { return conn_; }
    private:

        MYSQL* conn_;
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
        void init(const std::string &url, const std::string &user, const std::string &password, const std::string &dbname, int port, int max_conn, fleabane::ThreadPool* businessPool) {
            m_url = url;
            m_user = user;
            m_password = password;
            m_dbname = dbname;
            m_port = port;
            mMaxConn_ = max_conn;

            mBusinessPool_ = businessPool;

            // 在创建之前，加锁以保证线程安全
            std::lock_guard<std::mutex> lock(m_mutex);

            // 循环创建max_conn个数量的数据库连接
            for(int i = 0; i < mMaxConn_; ++i) {
                MYSQL * init_conn = mysql_init(nullptr);

                // 检查是否初始化成功
                if(!init_conn) {
                    throw std::runtime_error("MySQL Error: mysql_init() failed.");
                }

                // 尝试建立真正的物理连接
                MYSQL* connect_result = mysql_real_connect(init_conn, m_url.c_str(), m_user.c_str(), m_password.c_str(), m_dbname.c_str(), m_port, nullptr, 0);

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

                // 创建成功则加入队列
                m_conns.push_back(init_conn);
            }

            // 更新信号量，通知有m_max_conn个资源可用
            if(mMaxConn_ > 0) {
                m_reserve.release(mMaxConn_);
            }
        }

        /// 销毁连接池
        void destroy_pool() {
            // 上锁，确保在销毁的过程中没有其他线程在操作队列
            std::lock_guard<std::mutex> lock(m_mutex);

            // 循环遍历并关闭池中所有的空闲连接
            while(!m_conns.empty()) {
                MYSQL* conn = m_conns.front();
                m_conns.pop_front();

                mysql_close(conn);
            }
        } // m_reserve的析构函数会自动清理其资源，因此无需我们手动操作

        /// 获取连接，需要使用异步方式
        Task<std::shared_ptr<IDbConnection>> getConnectionAsync() override {
            // 返回 Awaiter，触发 co_await 机制
            // 这里还需要使用co_return，因为我们有返回值，这个返回值就是await_resume()的结果（一个连接），它会通过return_value()传递给Task，又通过Task.await_resume()返回出去
            MYSQL* raw_conn = co_await ConnectionAwaiter{*this};

            // 创建MySQLConnection
            IDbConnection* conn_wrapper = new MySQLConnection(raw_conn);

            // 封装成shared_ptr
            std::shared_ptr<IDbConnection> conn_ptr(
                conn_wrapper,
                [this](IDbConnection *p) {
                    // 自定义删除器
                    // 下转型
                    auto mysql_conn = dynamic_cast<MySQLConnection*>(p);
                    if(mysql_conn == nullptr) {
                        return;
                    }
                    // 取出物理连接
                    MYSQL * raw_conn = mysql_conn->raw();
                    // 归还给连接池
                    this->releaseConnection(raw_conn);
                    // 销毁wrapper本身
                    delete p;
                }
            );

            // 返回出去
            co_return conn_ptr;
        }


    private:

        // 构造函数和析构函数设置为私有，放置被外界创建和销毁
        // 这里设置为20，但是实际上要更大（CPU核数*2+磁盘IO等待系数，或者直接等于最大连接数）
        AsyncMySQLPool() : m_reserve(0), mBlockingPool_(std::make_unique<fleabane::ThreadPool>(20)) {}
        ~AsyncMySQLPool() override {
            destroy_pool();
        }

        /// 将任务投递到 DB 专用的阻塞线程池
        void postToBlockingPool(std::function<void()> task) const {
            mBlockingPool_->enqueue(std::move(task));
        }

        /// 获取业务线程池
        fleabane::ThreadPool* getBusinessPool() {
            return mBusinessPool_;
        }

        // ==========================================
        // 核心改造点：自定义 Awaiter
        // ==========================================
        struct ConnectionAwaiter {
            AsyncMySQLPool& pool_;
            MYSQL* conn_ = nullptr; // 用于传递结果

            // 1. 是否需要挂起？
            bool await_ready() {
                std::lock_guard<std::mutex> lock(pool_.m_mutex);
                // 如果有空闲连接，就不挂起，直接拿走
                if (!pool_.m_conns.empty()) {
                    conn_ = pool_.m_conns.front();
                    pool_.m_conns.pop_front();
                    return true; // true = 不挂起
                }
                return false; // false = 挂起，进入 await_suspend
            }

            // 2. 挂起时的逻辑
            void await_suspend(std::coroutine_handle<> cur_handle) {
                std::lock_guard<std::mutex> lock(pool_.m_mutex);
                // 再次检查（双重检查），防止挂起瞬间刚好有连接归还
                if (!pool_.m_conns.empty()) {
                    conn_ = pool_.m_conns.front();
                    pool_.m_conns.pop_front();
                    cur_handle.resume(); // 立即恢复
                    return;
                }

                // 真的没连接了，把句柄和"我"（Awaiter本身）存入等待队列
                // 为什么存 Awaiter 指针？因为我们需要把连接塞回 conn_ 成员变量里传给协程
                pool_.m_waiters.push_back({cur_handle, this});
            }

            // 3. 恢复后拿结果
            MYSQL* await_resume() {
                return conn_;
            }
        };

        /// 归还连接
        /// 不由业务逻辑线程调用，而是由智能指针管理
        void releaseConnection(MYSQL* conn) {
            Waiter waiter = {nullptr, nullptr}; // 临时保存等待者

            {
                // 1. 最小化锁的范围，只保护队列操作
                std::lock_guard<std::mutex> lock(m_mutex);

                if (!m_waiters.empty()) {
                    waiter = m_waiters.front();
                    m_waiters.pop_front();
                } else {
                    // 没人排队，归还连接
                    m_conns.push_back(conn);
                }
            } // 锁在这里释放

            // 2. 在锁外处理恢复逻辑
            if (waiter.handle != nullptr && waiter.awaiter != nullptr) {
                // 将连接传递给等待者
                waiter.awaiter->conn_ = conn;

                // 【关键修复】打断递归！
                // 不要直接 waiter.handle.resume();
                // 而是扔回业务线程池调度
                if (mBusinessPool_) {
                    mBusinessPool_->enqueue([h = waiter.handle]() {
                        h.resume();
                    });
                } else {
                    // 兜底：如果没有业务线程池（极端情况），才不得不直接 resume
                    // 但在你的架构里应该总是有 pool 的
                    waiter.handle.resume();
                }
            }
        }


        // 数据库信息
        std::string m_url; // 数据库地址
        std::string m_user; // 用户名
        std::string m_password; // 密码
        std::string m_dbname; // 数据库名称
        int m_port = 0; // 端口号

        // 连接池信息
        int mMaxConn_ = 0; // 最大连接数量
        int mCurConn_ = 0; // 当前已使用的连接数
        int mFreeConn_ = 0; // 当前空闲的连接数

        std::unique_ptr<fleabane::ThreadPool> mBlockingPool_; // 专用于DB的阻塞线程池，当查询需要阻塞时，先加到其中
        fleabane::ThreadPool* mBusinessPool_ = nullptr;

        std::mutex m_mutex; // 互斥锁
        std::counting_semaphore<> m_reserve; // 信号量默认被初始化为0，将来会在init函数中被初始化为最大连接个数
        std::list<MYSQL*> m_conns; // 空闲连接

        // 支持协程的异步操作
        struct Waiter {
            std::coroutine_handle<> handle;
            ConnectionAwaiter* awaiter;
        };
        std::deque<Waiter> m_waiters; // 等待连接的协程队列

        friend struct ConnectionAwaiter;
        friend class MySQLConnection;

    };


    // 查询的实现
    // 在头文件的（类外部）定义函数要显式指定inline
    inline Task<DBResult> MySQLConnection::query(std::string sql) {
        // 定义一个 Awaiter，把任务抛给 DB 专用线程池
        struct QueryAwaiter {
            std::string sql_;
            MYSQL* conn_;
            DBResult result_;
            std::exception_ptr ex_;

            bool await_ready() { return false; } // 总是挂起

            void await_suspend(std::coroutine_handle<> h) {
                auto& pool = AsyncMySQLPool::get_instance();
                auto businessPool = AsyncMySQLPool::get_instance().getBusinessPool();

                // 获取全局的 DB 线程池（注意：不是业务 Worker 池，是专门做慢 IO 的）
                // 假设我们有一个 global_db_thread_pool
                pool.postToBlockingPool([this, h, businessPool]() {
                    // --- 这里是 DB 线程 ---

                    // 1. 执行阻塞的 mysql_query
                    try {
                        if (mysql_query(conn_, sql_.c_str())) {
                            throw std::runtime_error("MySQL Query Error: " + std::string(mysql_error(conn_)));
                        }

                        // 2. 尝试获取结果集 (SELECT 等)
                        MYSQL_RES* res = mysql_store_result(conn_);

                        if (res) {
                            // --- 情况 A: 有结果集 (SELECT, SHOW 等) ---

                            // A-1. 获取行数和列数
                            unsigned int num_fields = mysql_num_fields(res);
                            my_ulonglong num_rows = mysql_num_rows(res);

                            // A-2. 获取列名 (Metadata)
                            MYSQL_FIELD* fields = mysql_fetch_fields(res);
                            result_.columns.reserve(num_fields);
                            for (unsigned int i = 0; i < num_fields; i++) {
                                result_.columns.emplace_back(fields[i].name);
                            }

                            // A-3. 获取行数据
                            result_.rows.reserve(num_rows); // 预分配行空间

                            MYSQL_ROW row;
                            while ((row = mysql_fetch_row(res))) {
                                std::vector<std::string> row_data;
                                row_data.reserve(num_fields); // 预分配列空间

                                // 获取当前行每一列的长度（重要：用于处理二进制数据和防止 \0 截断）
                                unsigned long* lengths = mysql_fetch_lengths(res);

                                for (unsigned int i = 0; i < num_fields; i++) {
                                    if (row[i]) {
                                        // 构造 string，指定长度
                                        row_data.emplace_back(row[i], lengths[i]);
                                    } else {
                                        // 处理数据库 NULL 值，这里默认转为空字符串
                                        // 也可以约定一个特殊标记，或者 DBResult 使用 std::optional<string>
                                        row_data.emplace_back("");
                                    }
                                }
                                result_.rows.push_back(std::move(row_data));
                            }

                            // 释放结果集内存
                            mysql_free_result(res);

                        } else {
                            // --- 情况 B: 无结果集 (INSERT, UPDATE, DELETE) 或 错误 ---

                            // mysql_field_count 返回应该有的列数
                            // 如果为 0，说明原本就是没有结果集的操作，成功
                            // 如果 > 0，说明应该有结果集但 mysql_store_result 返回了 NULL，这是个错误
                            if (mysql_field_count(conn_) == 0) {
                                // 获取受影响行数
                                result_.affectedRows = mysql_affected_rows(conn_);
                                // 获取自增ID
                                result_.insertId = mysql_insert_id(conn_);
                            } else {
                                throw std::runtime_error("MySQL Store Result Error: " + std::string(mysql_error(conn_)));
                            }
                        }

                    } catch (...) {
                        // 捕获异常
                        ex_ = std::current_exception();
                    }

                    // 3. 【关键】恢复协程
                    // 这里可以选择直接 resume（业务逻辑将在 DB 线程继续跑）
                    // 也可以把 h 扔回 Worker 线程池（建议这样做，保持线程职责单一）
                    if(businessPool) {
                        businessPool->enqueue([h](){
                             h.resume();
                        });
                    } else {
                        // 如果没有设置业务线程池，则报错
                        LOG_INFO("error");
                    }
                });
            }

            DBResult await_resume() {
                if(ex_) {
                    std::rethrow_exception(ex_);
                }
                return result_;
            }
        };

        co_return co_await QueryAwaiter{sql, conn_};
    }

}

#endif //ASYNCMYSQLPOOL_H
