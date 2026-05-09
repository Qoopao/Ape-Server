#include <cstddef>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/builder/stream/array.hpp>
#include <bsoncxx/types.hpp>
#include <bsoncxx/json.hpp>

#include <spdlog/spdlog.h>
#include <string>
#include <vector>
#include <optional>

#include "util/redisconnector.h"
#include "util/redishandler.h"
#include "user/userinfo.h"
#include "messagequeue/kafkaproducer.h"


bool RedisHandler::MsgToMQ(std::string key, std::string msgid){
    // 调用生产者生产消息
    KafkaProducer producer("msg_topic");
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
