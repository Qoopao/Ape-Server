#ifndef REDISHANDLER_H
#define REDISHANDLER_H

#include <sdkws.pb.h>
#include <util/redisconnector.h>

#include <optional>
#include <string>
#include <vector>

#include <boost/asio.hpp>

// ============================================================================
// RedisHandler — 基于 RedisConnector 的异步业务层
//
// 所有方法返回 boost::asio::awaitable<T>，由上层 gRPC handler co_await 调用
//
// 全量消息缓存方案（在线/离线统一）：
//   Redis Sorted Set：key = user_msgs:{userId}
//   score = msg seq（int64_t），member = 序列化的 sdkws::MsgData（base64 编码）
//   消息出生即入 ZSET，不再区分在线/离线路径。
// ============================================================================

class RedisHandler {
public:
    // ── 消息存取 ──
    // 将完整消息存入 Redis
    static boost::asio::awaitable<bool>
        SaveMsg(const sdkws::MsgData &msg);

    // 从 Redis 获取完整消息
    static boost::asio::awaitable<std::optional<sdkws::MsgData>>
        GetMsg(const std::string &msgid);

    // 删除消息缓存（DEL msg:{msgid}，用于延迟双删的第一步）
    static boost::asio::awaitable<void> DeleteMsgCache(const std::string &msgid);

    // ── 全量消息缓存（user_msgs ZSET，在线/离线统一）──
    // 消息出生即入 ZSET（SendMessages 调用），score = seq
    static boost::asio::awaitable<bool>
        SaveOfflineMsg(const std::string &userId, const sdkws::MsgData &msg);

    // 从 Redis user_msgs ZSET 按 seq 范围拉取消息
    static boost::asio::awaitable<std::vector<sdkws::MsgData>>
        GetOfflineMsgs(const std::string &userId, int64_t afterSeq, int limit,
                       const std::string &conversationID = {});

    // 从 ZSET 删除消息
    static boost::asio::awaitable<bool>
        DeleteOfflineMsgs(const std::string &userId,
                          const std::vector<std::string> &msgIds);

    // ACK 单个消息（从 user_msgs ZSET 删除）
    static boost::asio::awaitable<bool>
        AckOfflineMsg(const std::string &userId, const std::string &msgId);

    // 批量 ACK 消息（从 user_msgs ZSET 删除）
    static boost::asio::awaitable<bool>
        AckOfflineMsgsBatch(const std::string &userId,
                            const std::vector<std::string> &msgIds);

    // 幂等标记结果
    enum class ConsumeMarkResult {
        FirstTime,    // SET NX 成功，首次消费
        Duplicate,    // key 已存在，重复消息
        Unavailable   // Redis 不可用，需降级兜底
    };

    // 幂等：标记消息已消费
    static boost::asio::awaitable<ConsumeMarkResult>
        TryMarkMsgConsumed(const std::string &serverMsgID);

    // 幂等：删除消费标记（处理失败时回滚，允许重试）
    static boost::asio::awaitable<void>
        DeleteMsgConsumed(const std::string &serverMsgID);

    // ── 群成员缓存 ──
    static boost::asio::awaitable<void>
        CacheGroupMembers(const std::string &groupID,
                          const std::vector<std::string> &memberIDs);

    static boost::asio::awaitable<std::vector<std::string>>
        GetGroupMembersFromCache(const std::string &groupID);

    static boost::asio::awaitable<int64_t>
        GetGroupMemberCountFromCache(const std::string &groupID);

    static boost::asio::awaitable<void>
        InvalidateGroupCache(const std::string &groupID);

    // ── 批量 ZADD（群消息 fan-out 用 Lua 脚本）──
    // 将同一条 msg 批量写入多个 user_msgs:{userID} ZSET
    static boost::asio::awaitable<long long>
        BatchSaveOfflineMsg(const std::vector<std::string> &userIDs,
                            const sdkws::MsgData &msg);

private:
    // ZSET key for offline messages
    static std::string offlineMsgKey(const std::string &userId);

    // Serialize MsgData to base64 string for ZSET member
    static std::string serializeMsg(const sdkws::MsgData &msg);

    // Deserialize base64 string back to MsgData
    static std::optional<sdkws::MsgData> deserializeMsg(const std::string &data);
};

#endif