#include "storage/redishandler.h"
#include "storage/redisconnector.h"
#include <algorithm>
#include <spdlog/spdlog.h>

// Redis caches message bodies for Kafka dispatch. Offline recovery queries Mongo
// and checks durable MySQL receipts; no user_msgs ZSET is written or read here.
boost::asio::awaitable<bool>
RedisHandler::SaveMsg(const sdkws::MsgData &msg) {
    try {
        auto &redis = RedisConnector::instance();
        std::string key = "msg:" + msg.servermsgid();
        std::string value;
        msg.SerializeToString(&value);
        co_await redis.setex(key, 7 * 86400, value);
        co_return true;
    } catch (const std::exception &e) {
        spdlog::error("RedisHandler::SaveMsg failed: {}, serverMsgID={}", e.what(), msg.servermsgid());
        co_return false;
    }
}

// ──────────────────────────────────────────
// 从 Redis 获取完整消息
// ──────────────────────────────────────────
boost::asio::awaitable<std::optional<sdkws::MsgData>>
RedisHandler::GetMsg(const std::string &msgid) {
    try {
        auto &redis = RedisConnector::instance();
        std::string key = "msg:" + msgid;
        auto value = co_await redis.get(key);
        if (value) {
            sdkws::MsgData msg;
            if (msg.ParseFromString(*value)) {
                co_return msg;
            }
        }
        co_return std::nullopt;
    } catch (const std::exception &e) {
        spdlog::error("RedisHandler::GetMsg failed: {}", e.what());
        co_return std::nullopt;
    }
}

boost::asio::awaitable<void>
RedisHandler::DeleteMsgCache(const std::string &msgid) {
    auto &redis = RedisConnector::instance();
    std::string key = "msg:" + msgid;
    co_await redis.del(key);
}


boost::asio::awaitable<void> RedisHandler::CleanupAcknowledgedMessage(uint64_t owner,const std::string& id) {
    auto& redis=RedisConnector::instance();
    co_await DeleteMsgCache(id);
    // Only remove metadata of this confirmed message; never erase an entire user queue.
    co_await redis.del("msg_conv:"+id);
    co_await redis.del("msg:consumed:"+id);
    co_await redis.del("pending_ack:online:"+id);
    co_await redis.del("pending_ack:offline:"+std::to_string(owner)+":"+id);
    co_await redis.zrem("pending_ack:deadlines",id);
}
boost::asio::awaitable<void> RedisHandler::DeleteSingleChatHistory(uint64_t first,uint64_t second,
    std::vector<std::string> ids) {
    // Caller selects Mongo message IDs at/below deletion cutoff. New messages are preserved.
    std::sort(ids.begin(),ids.end()); ids.erase(std::unique(ids.begin(),ids.end()),ids.end());
    for(const auto& id:ids) {
        co_await CleanupAcknowledgedMessage(first,id);
        co_await CleanupAcknowledgedMessage(second,id);
    }
}
