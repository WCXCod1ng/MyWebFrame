//
// Created by user on 2025/12/19.
//

#ifndef DBINTERFACES_H
#define DBINTERFACES_H
#include <memory>
#include <string>
#include <vector>

#include "coroutine/Coroutine.h"

namespace sedum {
    // 1. 抽象的查询结果
    struct DBResult {
        // === 元数据 ===
        unsigned long long affectedRows = 0;       // 受影响的行数 (INSERT/UPDATE/DELETE)
        unsigned long long insertId = 0;  // 自增ID (INSERT)

        // === 结果集 (SELECT) ===
        std::vector<std::string> columns; // 列名
        std::vector<std::vector<std::string>> rows; // 数据行

        // 辅助：判断是否是 SELECT 结果
        bool isSelect() const {
            return !columns.empty();
        }

        // 辅助：获取某一行某一列的值
        std::string getValue(size_t rowIndex, size_t colIndex) const {
            if (rowIndex < rows.size() && colIndex < rows[rowIndex].size()) {
                return rows[rowIndex][colIndex];
            }
            return "";
        }
    };

    // 2. 抽象的数据库操作接口
    class IDbConnection {

    public:
        virtual ~IDbConnection() = default;

        // 执行查询，返回 Task 以支持协程
        // 这里我们假设 Query 是一个通用的操作
        virtual Task<DBResult> query(std::string sql) = 0;
    };

    // 3. 抽象的连接池接口
    class IDbPool {
    public:
        virtual ~IDbPool() = default;

        // 获取连接，返回一个 Awaitable 对象，co_await 后得到 IDbConnection 的智能指针
        virtual Task<std::shared_ptr<IDbConnection>> getConnectionAsync() = 0;
    };
}

#endif //DBINTERFACES_H
