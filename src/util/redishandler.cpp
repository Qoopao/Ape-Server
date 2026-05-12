#include <cstddef>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/builder/stream/array.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/json.hpp>

#include <spdlog/spdlog.h>
#include <string>
#include <vector>
#include <optional>
#include <algorithm>
#include <unordered_map>

#include "util/redisconnector.h"
#include "util/redishandler.h"
#include "user/userinfo.h"
#include "messagequeue/kafkaproducer.h"


bool RedisHandler::MsgToMQ(std::string key, std::string msgid){
    // 调用生产者生产消息 → msg_topic（在线实时推送）
    KafkaProducer producer("msg_topic");
    return producer.Deliver(key, msgid.data(), msgid.size());
}

bool RedisHandler::MsgToOfflineMQ(std::string key, std::string msgid){
    // 调用生产者生产消息 → offline_topic（离线持久化）
    KafkaProducer producer("offline_msg_topic");
    return producer.Deliver(key, msgid.data(), msgid.size());
}

bool RedisHandler::SaveMsgInfo(sdkws::MsgData msg){
    // 把完整消息存入Redis
    auto& redis = RedisConnector::get_instance().get_redis();

    auto key = msg.servermsgid();
    try{
        auto data = msg.SerializeAsString();
        return redis.set(key, data,std::chrono::milliseconds(0), UpdateType::NOT_EXIST);
    }catch(...){
        spdlog::error("SaveMsgInfo SerializeAsString failed");
        return false;
    }

}

std::optional<sdkws::MsgData> RedisHandler::GetMsgInfo(std::string msgid){
    // 从Redis获得完整消息
    auto& redis = RedisConnector::get_instance().get_redis();

    auto key = msgid;
    auto data = redis.get(key);
    if(data->empty()){
        spdlog::warn("GetMsgInfo from Redis failed");
        return std::nullopt;
    }

    try{
        sdkws::MsgData msg;
        msg.ParseFromString(data.value());
        return msg;
    }catch(...){
        spdlog::error("GetMsgInfo ParseFromString failed");
        return std::nullopt;
    }
}

// ==================== 离线消息热存储（Redis Hash） ====================

bool RedisHandler::SaveOfflineMsg(const std::string& userId, const sdkws::MsgData& msg){
    auto& redis = RedisConnector::get_instance().get_redis();
    std::string offlineKey = "offline:" + userId;

    try{
        auto data = msg.SerializeAsString();
        // 写入 Hash: field=serverMsgID, value=MsgData序列化字节
        redis.hset(offlineKey, msg.servermsgid(), data);
        // 设置7天TTL（仅在首次创建key时设置）
        redis.expire(offlineKey, std::chrono::seconds(7 * 24 * 3600));

        spdlog::info("SaveOfflineMsg success, userId={}, msgId={}, seq={}", userId, msg.servermsgid(), msg.seq());
        return true;
    }catch(...){
        spdlog::error("SaveOfflineMsg failed, userId={}, msgId={}", userId, msg.servermsgid());
        return false;
    }
}

std::vector<sdkws::MsgData> RedisHandler::GetOfflineMsgs(const std::string& userId, int64_t afterSeq, int limit, const std::string& conversationID){
    auto& redis = RedisConnector::get_instance().get_redis();
    std::string offlineKey = "offline:" + userId;

    std::vector<sdkws::MsgData> result;
    try{
        // 获取Hash所有field-value（使用redis++的output iterator接口）
        std::unordered_map<std::string, std::string> fields;
        redis.hgetall(offlineKey, std::inserter(fields, fields.begin()));

        for(auto& [field, value] : fields){
            try{
                sdkws::MsgData msg;
                msg.ParseFromString(value);
                // 只拉取 seq > afterSeq 的消息
                if(msg.seq() > afterSeq){
                    // conversationID 过滤（空字符串表示不过滤）
                    if(conversationID.empty()){
                        result.push_back(std::move(msg));
                    } else{
                        std::string convID = msg.convid().empty() ? ("single_" + msg.recvid()) : msg.convid();
                        if(convID == conversationID || msg.convid() == conversationID){
                            result.push_back(std::move(msg));
                        }
                    }
                }
            }catch(...){
                spdlog::error("GetOfflineMsgs ParseFromString failed for field={}", field);
            }
        }

        // 按seq升序排序
        std::sort(result.begin(), result.end(),
            [](const sdkws::MsgData& a, const sdkws::MsgData& b){
                return a.seq() < b.seq();
            });

        // 截取limit条
        if((int)result.size() > limit){
            result.resize(limit);
        }

        return result;
    }catch(...){
        spdlog::error("GetOfflineMsgs failed, userId={}", userId);
        return result;
    }
}

bool RedisHandler::DeleteOfflineMsgs(const std::string& userId, const std::vector<std::string>& msgIds){
    auto& redis = RedisConnector::get_instance().get_redis();
    std::string offlineKey = "offline:" + userId;

    try{
        for(auto& msgId : msgIds){
            redis.hdel(offlineKey, msgId);
        }
        spdlog::info("DeleteOfflineMsgs success, userId={}, count={}", userId, msgIds.size());
        return true;
    }catch(...){
        spdlog::error("DeleteOfflineMsgs failed, userId={}", userId);
        return false;
    }
}

// ==================== 离线消息 ACK 确认（拉取后标记已推送） ====================

bool RedisHandler::AckOfflineMsg(const std::string& userId, const std::string& msgId){
    auto& redis = RedisConnector::get_instance().get_redis();
    std::string offlineKey = "offline:" + userId;
    std::string ackKey = "offline_acked:" + userId;

    try{
        // 1. 将已确认的消息移动到 acked set（供后续 MongoDB 标记已推送）
        auto msgData = redis.hget(offlineKey, msgId);
        if(msgData){
            redis.sadd(ackKey, msgId);
            redis.expire(ackKey, std::chrono::seconds(7 * 24 * 3600));
        }
        // 2. 从热数据Hash中删除
        redis.hdel(offlineKey, msgId);
        return true;
    }catch(...){
        spdlog::error("AckOfflineMsg failed, userId={}, msgId={}", userId, msgId);
        return false;
    }
}

bool RedisHandler::AckOfflineMsgsBatch(const std::string& userId, const std::vector<std::string>& msgIds){
    if(msgIds.empty()) return true;

    auto& redis = RedisConnector::get_instance().get_redis();
    std::string offlineKey = "offline:" + userId;
    std::string ackKey = "offline_acked:" + userId;

    try{
        // 批量确认
        std::vector<std::string> hdelFields = msgIds;
        for(auto& msgId : msgIds){
            auto msgData = redis.hget(offlineKey, msgId);
            if(msgData){
                redis.sadd(ackKey, msgId);
            }
        }
        redis.expire(ackKey, std::chrono::seconds(7 * 24 * 3600));

        // 批量从Hash删除
        if(!msgIds.empty()){
            redis.hdel(offlineKey, msgIds.begin(), msgIds.end());
        }

        spdlog::info("AckOfflineMsgsBatch success, userId={}, count={}", userId, msgIds.size());
        return true;
    }catch(...){
        spdlog::error("AckOfflineMsgsBatch failed, userId={}", userId);
        return false;
    }
}
