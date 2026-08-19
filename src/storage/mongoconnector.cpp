#include "storage/mongoconnector.h"

#include <bsoncxx/builder/stream/document.hpp>
#include <mongocxx/exception/exception.hpp>
#include <spdlog/spdlog.h>
#include <string>

MongoConnector &MongoConnector::instance()
{
    static MongoConnector instance;
    return instance;
}

MongoConnector::~MongoConnector()
{
    stop();
}

void MongoConnector::start(int pool_size)
{
    if (_running)
    {
        spdlog::warn("MongoConnector already running (pool_size={})", _poolSize);
        return;
    }

    _poolSize = pool_size;
    _iocPool = std::make_unique<IOC_Pool>(pool_size);
    _mongoPools.reserve(pool_size);

    // 每个 worker 绑定一个独立的 mongocxx::pool（对标 MySQL 的 per-worker connection_pool）
    for (int i = 0; i < pool_size; ++i)
    {
        std::string uri_string = "mongodb://" + _dbUsername + ":" + _dbPassword +
                                 "@localhost:27017/?authSource=admin"
                                 "&minPoolSize=" + std::to_string(_minPoolSize) +
                                 "&maxPoolSize=" + std::to_string(_maxPoolSize);
        mongocxx::uri uri(uri_string);

        try
        {
            auto pool = std::make_shared<mongocxx::pool>(uri);

            // 从该 pool 取出一个 client 测试连接
            auto client = pool->acquire();
            bsoncxx::builder::stream::document ping_cmd;
            ping_cmd << "ping" << 1;
            client["admin"].run_command(ping_cmd.view());

            _mongoPools.emplace_back(std::move(pool));
            spdlog::info("MongoConnector pool[{}] created (min={}, max={})",
                         i, _minPoolSize, _maxPoolSize);
        }
        catch (const mongocxx::exception &e)
        {
            spdlog::error("MongoConnector pool[{}] failed to connect: {}", i, e.what());
            throw;
        }
    }

    _iocPool->startPool();
    _running = true;

    spdlog::info("MongoConnector started (IOC_Pool, pool_size={})", pool_size);
}

void MongoConnector::stop()
{
    if (!_running) return;

    _iocPool->stopPool();
    _mongoPools.clear();
    _poolSize = 0;
    _running = false;

    spdlog::info("MongoConnector pool stopped");
}

int MongoConnector::pool_size() const { return _poolSize; }

boost::asio::io_context::executor_type
MongoConnector::get_executor()
{
    return _iocPool->get_ioc(next_idx()).get_executor();
}

mongocxx::pool::entry MongoConnector::acquire_client()
{
    int idx = next_idx();
    return _mongoPools[idx]->acquire();
}

int MongoConnector::next_idx()
{
    return _nextIdx.fetch_add(1, std::memory_order_relaxed) % _poolSize;
}