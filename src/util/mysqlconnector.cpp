#include "util/mysqlconnector.h"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/mysql/connection_pool.hpp>
#include <boost/mysql/results.hpp>
#include <chrono>
#include <spdlog/spdlog.h>

MySQLConnector &MySQLConnector::instance()
{
    static MySQLConnector instance;
    return instance;
}

MySQLConnector::~MySQLConnector()
{
    stop();
}

void MySQLConnector::start(int pool_size)
{
    if (_running)
    {
        spdlog::warn("MySQLConnector already running (pool_size={})", _poolSize);
        return;
    }

    _poolSize = pool_size;
    _iocPool = std::make_unique<IOC_Pool>(pool_size);
    _pools.resize(pool_size);

    // 每个 worker 绑一个独立的 connection_pool，实现真正的多线程并行 MySQL I/O
    // （对标 RedisConnector：每个 worker 一个 connection）
    for (int i = 0; i < pool_size; ++i)
    {
        auto &ioc = _iocPool->get_ioc(i);

        boost::mysql::pool_params params;
        params.server_address.emplace_host_and_port("127.0.0.1", 3306);
        params.username = "ape_user";
        params.password = "ape_password";
        params.database = "ape_auth";
        params.ssl = boost::mysql::ssl_mode::disable;

        // 按 worker 数等分连接上限，避免总连接数膨胀
        // 每个 pool 至少保留 1 个 initial / 2 个 max
        int per_pool_initial = std::max(1, 2 / pool_size);
        int per_pool_max = std::max(2, 20 / pool_size);
        params.initial_size = per_pool_initial;
        params.max_size = per_pool_max;

        _pools[i] = std::make_unique<boost::mysql::connection_pool>(
            boost::asio::any_io_executor(ioc.get_executor()),
            std::move(params));

        // 启动连接池（运行在该 worker 的 io_context 上）
        _pools[i]->async_run(boost::asio::detached);

        spdlog::info("MySQLConnector pool[{}] created (initial={}, max={})",
                     i, per_pool_initial, per_pool_max);
    }

    _iocPool->startPool();
    _running = true;

    spdlog::info("MySQLConnector started (IOC_Pool, pool_size={})", pool_size);
}

void MySQLConnector::stop()
{
    if (!_running) return;

    _iocPool->stopPool();
    _pools.clear();
    _poolSize = 0;
    _running = false;

    spdlog::info("MySQLConnector pool stopped");
}

int MySQLConnector::pool_size() const { return _poolSize; }

boost::asio::io_context::executor_type
MySQLConnector::get_executor()
{
    return _iocPool->get_ioc(next_idx()).get_executor();
}

int MySQLConnector::next_idx()
{
    return _nextIdx.fetch_add(1, std::memory_order_relaxed) % _poolSize;
}