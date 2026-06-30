#ifndef MONGOCONNECTOR_H
#define MONGOCONNECTOR_H

#include "util/ioc_pool.h"

#include <atomic>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include <boost/asio.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/pool.hpp>
#include <mongocxx/uri.hpp>

// ============================================================================
// MongoConnector — 基于 IOC_Pool 的 MongoDB 异步连接器
//
// 架构（完全对标 MySQLConnector / RedisConnector）：
//   - 内部使用 IOC_Pool 管理 io_context 池和线程生命周期
//   - 每个 IOC_Pool worker 绑定一个独立的 mongocxx::pool
//   - acquire_client() 通过轮询选取 worker 的 pool 获取连接
//   - 每个 worker 独立连接池，避免单 pool 瓶颈，真正多线程并行
//   - async_run<Func>() 将同步阻塞操作投递到 IOCPool worker 执行，
//     通过 co_await asio::post 切回调用方 executor 恢复协程
// ============================================================================

class MongoConnector
{
public:
    static MongoConnector &instance();

    void start(int pool_size = 4);
    void stop();

    int pool_size() const;

    // 返回一个 executor，供 co_spawn 使用（轮询选取 io_context）
    boost::asio::io_context::executor_type get_executor();

    // 从轮询选中的 worker 的 mongocxx::pool 中获取一个 client
    mongocxx::pool::entry acquire_client();

    // 将同步阻塞的 MongoDB 操作投递到 IOCPool worker 执行，不阻塞调用方 io_context
    // 用法: co_await MongoConnector::instance().async_run([&]{ return some_sync_op(); });
    template <typename Func>
    boost::asio::awaitable<std::invoke_result_t<Func>> async_run(Func &&func)
    {
        // 保存调用方 executor
        auto caller_executor = co_await boost::asio::this_coro::executor;

        // 切换到 MongoDB worker 线程
        co_await boost::asio::post(
            boost::asio::bind_executor(get_executor(), boost::asio::use_awaitable));

        // 在 MongoDB worker 上执行同步阻塞操作
        if constexpr (std::is_void_v<std::invoke_result_t<Func>>)
        {
            func();
            // 切回调用方 executor
            co_await boost::asio::post(
                boost::asio::bind_executor(caller_executor, boost::asio::use_awaitable));
        }
        else
        {
            auto result = func();
            // 切回调用方 executor
            co_await boost::asio::post(
                boost::asio::bind_executor(caller_executor, boost::asio::use_awaitable));
            co_return result;
        }
    }

private:
    MongoConnector() = default;
    ~MongoConnector();
    MongoConnector(const MongoConnector &) = delete;
    MongoConnector &operator=(const MongoConnector &) = delete;

    // 轮询选取下一个 worker 索引
    int next_idx();

    std::unique_ptr<IOC_Pool> _iocPool;
    // 每个 worker 绑定一个独立的 mongocxx::pool（对标 MySQL 的 per-worker connection_pool）
    std::vector<std::shared_ptr<mongocxx::pool>> _mongoPools;
    mongocxx::instance _mongoInstance; // 必须与 driver 生命周期一致
    std::atomic<int> _nextIdx{0};
    int _poolSize{0};
    bool _running{false};

    // 配置
    std::string _dbUsername{"root"};
    std::string _dbPassword{"root"};
    uint32_t _minPoolSize{2}; // 每个 pool 的最小连接数
    uint32_t _maxPoolSize{5}; // 每个 pool 的最大连接数
};

#endif