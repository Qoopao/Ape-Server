#include "storage/mongoconnector.h"

#include <bsoncxx/builder/stream/document.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/options/index.hpp>
#include <spdlog/spdlog.h>
#include <string>
#include <stdexcept>
#include <iterator>

namespace {
bool isMessageIdKey(bsoncxx::document::view index) {
    auto keys=index["key"];
    if(!keys || keys.type()!=bsoncxx::type::k_document) return false;
    auto view=keys.get_document().view();
    if(std::distance(view.begin(),view.end())!=1) return false;
    auto direction=view["serverMsgID"];
    if(!direction) return false;
    if(direction.type()==bsoncxx::type::k_int32) return direction.get_int32().value==1;
    if(direction.type()==bsoncxx::type::k_int64) return direction.get_int64().value==1;
    if(direction.type()==bsoncxx::type::k_double) return direction.get_double().value==1;
    return false;
}
}
bool MongoConnector::IsFullUniqueMessageIdIndex(bsoncxx::document::view index) {
    if(!isMessageIdKey(index)) return false;
    auto unique=index["unique"];
    if(!unique || unique.type()!=bsoncxx::type::k_bool || !unique.get_bool().value) return false;
    if(index["partialFilterExpression"]) return false;
    auto sparse=index["sparse"];
    return !sparse || (sparse.type()==bsoncxx::type::k_bool && !sparse.get_bool().value);
}

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
                                 "&w=majority&journal=true&wtimeoutMS=10000"
                                 "&serverSelectionTimeoutMS=5000&connectTimeoutMS=5000"
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

    {
        auto client = _mongoPools.front()->acquire();
        auto messages = client["IM-System"]["msg"];
        messages.create_index(bsoncxx::builder::stream::document{}
            << "convID" << 1 << "seq" << 1 << bsoncxx::builder::stream::finalize);
        messages.create_index(bsoncxx::builder::stream::document{}
            << "recvID" << 1 << "seq" << 1 << bsoncxx::builder::stream::finalize);
        mongocxx::options::index unique;
        unique.unique(true);
        unique.name("message_server_id_unique");
        bool existingUniqueIndex=false;
        std::string incompatibleIndex;
        for(const auto& index:messages.list_indexes()) {
            if(!isMessageIdKey(index)) continue;
            auto nameValue=index["name"].get_string().value;
            const std::string name(nameValue.data(),nameValue.size());
            if(IsFullUniqueMessageIdIndex(index)) {
                spdlog::info("MongoConnector: reusing unique serverMsgID index '{}'",name);
                existingUniqueIndex=true;
                break;
            }
            incompatibleIndex=name;
        }
        if(!existingUniqueIndex) {
            if(!incompatibleIndex.empty()) {
                throw std::runtime_error("Existing serverMsgID index '"+incompatibleIndex+
                    "' is not a full unique index. An explicit index migration is required.");
            }
            messages.create_index(bsoncxx::builder::stream::document{}
                << "serverMsgID" << 1 << bsoncxx::builder::stream::finalize, unique);
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
