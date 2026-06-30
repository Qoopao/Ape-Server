#ifndef MYSQLCONNECTOR_H
#define MYSQLCONNECTOR_H

#include "util/ioc_pool.h"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/mysql/connection_pool.hpp>
#include <boost/mysql/results.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <spdlog/spdlog.h>

// ============================================================================
// MySQLConnector — 基于 IOC_Pool + connection_pool 的 MySQL 异步连接器
//
// 架构（对标 RedisConnector）：
//   - 内部使用 IOC_Pool 管理 io_context 池和线程生命周期
//   - 每个 IOC_Pool worker 绑定一个独立的 boost::mysql::connection_pool
//   - async_execute 通过 dispatch 切换到目标 worker，在该 worker 的 pool 上执行
//   - 零阻塞，全异步协程模型，真正的多线程并行 MySQL I/O
// ============================================================================

class MySQLConnector
{
public:
    static MySQLConnector &instance();

    void start(int pool_size = 2);
    void stop();

    int pool_size() const;

    // 返回一个 executor，供 co_spawn 使用（轮询选取 io_context）
    boost::asio::io_context::executor_type get_executor();

    // 异步执行 SQL 语句（支持 with_params 参数化查询和原始 SQL 字符串）
    template <typename Stmt>
    boost::asio::awaitable<boost::mysql::results>
        async_execute(const Stmt &stmt)
    {
        // 轮询选取一个 worker
        int idx = next_idx();
        auto &ioc = _iocPool->get_ioc(idx);
        auto &pool = *_pools[idx];

        // 切换到目标 worker 的 io_context
        co_await boost::asio::dispatch(
            boost::asio::bind_executor(ioc.get_executor(), boost::asio::use_awaitable));

        // 从该 worker 绑定的连接池获取连接（I/O 在同一个 io_context 上）
        auto conn = co_await pool.async_get_connection(
            boost::asio::use_awaitable);

        boost::mysql::results results;
        co_await conn->async_execute(stmt, results, boost::asio::use_awaitable);

        co_return results;
    }

private:
    MySQLConnector() = default;
    ~MySQLConnector();
    MySQLConnector(const MySQLConnector &) = delete;
    MySQLConnector &operator=(const MySQLConnector &) = delete;

    // 轮询选取下一个 worker 索引
    int next_idx();

    std::unique_ptr<IOC_Pool> _iocPool;
    std::vector<std::unique_ptr<boost::mysql::connection_pool>> _pools;
    std::atomic<int> _nextIdx{0};
    int _poolSize{0};
    bool _running{false};
};

#endif