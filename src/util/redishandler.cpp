#include "util/redishandler.h"
#include "util/redisconnector.h"

#include <optional>
#include <string>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast/core/detail/base64.hpp>

#include <spdlog/spdlog.h>

// ═══════════════════════════════════════════════════════════════════════════
// RedisHandler 实现 — 全异步 awaitable 接口
//
// 全量消息缓存于 Redis Sorted Set（在线/离线统一）：
//   key:    user_msgs:{userId}
//   score:  msg.seq() (int64_t)
//   member: base64(serialized sdkws::MsgData)
//
//   TTL 7天，MongoDB 做冷存储兜底。
//   在线消息不再单独走一条路径——消息出生即入 ZSET。
// ═══════════════════════════════════════════════════════════════════════════

// ──────────────────────────────────────────
// 内部工具
// ──────────────────────────────────────────

std::string RedisHandler::offlineMsgKey(const std::string &userId) {
    return "user_msgs:" + userId;
}

std::string RedisHandler::serializeMsg(const sdkws::MsgData &msg) {
    std::string raw;
    msg.SerializeToString(&raw);
    std::size_t encoded_len =
        boost::beast::detail::base64::encoded_size(raw.size());
    std::string encoded(encoded_len, '\0');
    boost::beast::detail::base64::encode(encoded.data(), raw.data(),
                                         raw.size());
    // encoded has trailing null from size calculation, trim if needed
    while (!encoded.empty() && encoded.back() == '\0') {
        encoded.pop_back();
    }
    return encoded;
}

std::optional<sdkws::MsgData>
RedisHandler::deserializeMsg(const std::string &data) {
    std::size_t decoded_len =
        boost::beast::detail::base64::decoded_size(data.size());
    std::string decoded(decoded_len, '\0');
    auto [written, _] = boost::beast::detail::base64::decode(
        decoded.data(), data.data(), data.size());
    decoded.resize(written);

    sdkws::MsgData msg;
    if (msg.ParseFromString(decoded)) {
        return msg;
    }
    return std::nullopt;
}

// ──────────────────────────────────────────
// 将完整消息存入 Redis
// ──────────────────────────────────────────
boost::asio::awaitable<bool>
RedisHandler::SaveMsg(const sdkws::MsgData &msg) {
    try {
        auto &redis = RedisConnector::instance();
        std::string key = "msg:" + msg.servermsgid();
        std::string value;
        msg.SerializeToString(&value);
        co_await redis.set(key, value);
        co_return true;
    } catch (const std::exception &e) {
        spdlog::error("RedisHandler::SaveMsg failed: {}", e.what());
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

// ──────────────────────────────────────────
// 将离线消息存入 Redis ZSET
// ──────────────────────────────────────────
boost::asio::awaitable<bool>
RedisHandler::SaveOfflineMsg(const std::string &userId,
                             const sdkws::MsgData &msg) {
    try {
        auto &redis = RedisConnector::instance();
        std::string zkey = offlineMsgKey(userId);
        std::string encoded = serializeMsg(msg);
        int64_t score = msg.seq();
        co_await redis.zadd(zkey, score, encoded);

        // 设置 7 天过期，避免无限增长
        co_await redis.expire(zkey, 7 * 24 * 3600);

        co_return true;
    } catch (const std::exception &e) {
        spdlog::error("RedisHandler::SaveOfflineMsg failed: {}", e.what());
        co_return false;
    }
}

// ──────────────────────────────────────────
// 从 Redis ZSET 拉取离线消息（按 seq 范围查询）
// ──────────────────────────────────────────
boost::asio::awaitable<std::vector<sdkws::MsgData>>
RedisHandler::GetOfflineMsgs(const std::string &userId, int64_t afterSeq,
                             int limit, const std::string &conversationID) {
    std::vector<sdkws::MsgData> result;
    try {
        auto &redis = RedisConnector::instance();
        std::string zkey = offlineMsgKey(userId);

        // 查询 seq > afterSeq 的消息，limit 条
        auto members =
            co_await redis.zrangebyscore(zkey, afterSeq + 1,
                                         std::numeric_limits<int64_t>::max(),
                                         0, limit);

        for (const auto &encoded : members) {
            auto msg = deserializeMsg(encoded);
            if (msg) {
                // 如果指定了 conversationID，进行过滤
                if (!conversationID.empty() &&
                    msg->convid() != conversationID) {
                    continue;
                }
                result.push_back(std::move(*msg));
            }
        }
    } catch (const std::exception &e) {
        spdlog::error("RedisHandler::GetOfflineMsgs failed: {}", e.what());
    }
    co_return result;
}

// ──────────────────────────────────────────
// 删除离线消息（按 msgId 列表）
// ──────────────────────────────────────────
boost::asio::awaitable<bool>
RedisHandler::DeleteOfflineMsgs(const std::string &userId,
                                const std::vector<std::string> &msgIds) {
    try {
        if (msgIds.empty()) {
            co_return true;
        }
        auto &redis = RedisConnector::instance();
        std::string zkey = offlineMsgKey(userId);

        // 先取出所有成员，按 msgId 匹配后删除
        auto allMembers = co_await redis.zrangebyscore(
            zkey, 0, std::numeric_limits<int64_t>::max());

        std::vector<std::string> toRemove;
        for (const auto &encoded : allMembers) {
            auto msg = deserializeMsg(encoded);
            if (!msg) continue;
            for (const auto &targetId : msgIds) {
                if (msg->servermsgid() == targetId) {
                    toRemove.push_back(encoded);
                    break;
                }
            }
        }

        if (!toRemove.empty()) {
            co_await redis.zrem_range(zkey, toRemove);
        }
        co_return true;
    } catch (const std::exception &e) {
        spdlog::error("RedisHandler::DeleteOfflineMsgs failed: {}", e.what());
        co_return false;
    }
}

// ──────────────────────────────────────────
// ACK 单个离线消息
// ──────────────────────────────────────────
boost::asio::awaitable<bool>
RedisHandler::AckOfflineMsg(const std::string &userId,
                            const std::string &msgId) {
    try {
        auto &redis = RedisConnector::instance();
        std::string zkey = offlineMsgKey(userId);

        // 取出所有成员，找到匹配的
        auto allMembers = co_await redis.zrangebyscore(
            zkey, 0, std::numeric_limits<int64_t>::max());

        for (const auto &encoded : allMembers) {
            auto msg = deserializeMsg(encoded);
            if (msg && msg->servermsgid() == msgId) {
                co_await redis.zrem(zkey, encoded);
                co_return true;
            }
        }
        co_return false; // 未找到
    } catch (const std::exception &e) {
        spdlog::error("RedisHandler::AckOfflineMsg failed: {}", e.what());
        co_return false;
    }
}

// ──────────────────────────────────────────
// 批量 ACK 离线消息
// ──────────────────────────────────────────
boost::asio::awaitable<bool>
RedisHandler::AckOfflineMsgsBatch(
    const std::string &userId, const std::vector<std::string> &msgIds) {
    try {
        if (msgIds.empty()) {
            co_return true;
        }
        auto &redis = RedisConnector::instance();
        std::string zkey = offlineMsgKey(userId);

        auto allMembers = co_await redis.zrangebyscore(
            zkey, 0, std::numeric_limits<int64_t>::max());

        std::vector<std::string> toRemove;
        // 将 msgIds 放入 set 加速查找
        std::unordered_set<std::string> idSet(msgIds.begin(), msgIds.end());

        for (const auto &encoded : allMembers) {
            auto msg = deserializeMsg(encoded);
            if (msg && idSet.count(msg->servermsgid()) > 0) {
                toRemove.push_back(encoded);
            }
        }

        if (!toRemove.empty()) {
            co_await redis.zrem_range(zkey, toRemove);
        }
        co_return true;
    } catch (const std::exception &e) {
        spdlog::error("RedisHandler::AckOfflineMsgsBatch failed: {}",
                      e.what());
        co_return false;
    }
}

boost::asio::awaitable<RedisHandler::ConsumeMarkResult>
RedisHandler::TryMarkMsgConsumed(const std::string &serverMsgID) {
    try {
        auto &redis = RedisConnector::instance();
        std::string key = "msg:consumed:" + serverMsgID;
        bool ok = co_await redis.setnxex(key, "1", 7 * 86400);
        co_return ok ? ConsumeMarkResult::FirstTime : ConsumeMarkResult::Duplicate;
    } catch (const std::exception &e) {
        spdlog::error("RedisHandler::TryMarkMsgConsumed failed: {}, "
                      "fallback to MongoDB, serverMsgID={}",
                      e.what(), serverMsgID);
        co_return ConsumeMarkResult::Unavailable;
    }
}

boost::asio::awaitable<void>
RedisHandler::DeleteMsgConsumed(const std::string &serverMsgID) {
    try {
        auto &redis = RedisConnector::instance();
        std::string key = "msg:consumed:" + serverMsgID;
        co_await redis.del(key);
    } catch (const std::exception &e) {
        spdlog::error("RedisHandler::DeleteMsgConsumed failed: {}, serverMsgID={}",
                      e.what(), serverMsgID);
    }
    co_return;
}