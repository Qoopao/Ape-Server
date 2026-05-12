#include "services/push_service/server.h"
#include "messagequeue/kafkaconsumer.h"
#include "services/backbon_service/client.h"
#include "services/gateway_push_service/client.h"
#include "services/push_service/push_msg_handler.h"
#include "services/push_service/offline_msg_handler.h"
#include "util/redisconnector.h"
#include "util/redishandler.h"
#include "util/mongohandler.h"
#include <spdlog/spdlog.h>

PushServer::PushServer(const std::string &service_name,
                       const std::string &listen_address)
    : BaseServiceServer<PushServer>(service_name, listen_address) {}

PushServer::~PushServer() {
    if (kafkaConsumer_) {
        kafkaConsumer_->shutdown();
    }
    if (kafka_thread_.joinable()) {
        kafka_thread_.join();
    }
    if (offlineConsumer_) {
        offlineConsumer_->shutdown();
    }
    if (offline_thread_.joinable()) {
        offline_thread_.join();
    }
}

void PushServer::Start(const std::string &kafka_group_id,
                       const std::vector<std::string> &kafka_topics,
                       const std::vector<std::string> &offline_topics) {

    // 先启动 gRPC 服务器（继承自 BaseServiceServer）
    BaseServiceServer<PushServer>::Start();

    // 创建PushHandler，gRPC调用本服务的PushMsg
    auto pushHandler = std::make_shared<PushHandler>(
        grpc::CreateChannel(GetListenAddress(), grpc::InsecureChannelCredentials()));

    // 在独立线程中启动Kafka消费者（在线推送），避免阻塞主线程和 gRPC 服务器
    kafkaConsumer_ = std::make_unique<KafkaConsumer>(
        kafka_group_id, kafka_topics);
    kafkaConsumer_->setHandler(pushHandler);

    kafka_thread_ = std::thread([this]() {
        kafkaConsumer_->start();
    });

    spdlog::info("PushServer: Kafka consumer started in background thread, group={}, topics_count={}",
                kafka_group_id, kafka_topics.size());

    // 启动离线消息 Kafka 消费者（离线持久化）
    if (!offline_topics.empty()) {
        auto offlineHandler = std::make_shared<OfflineMsgHandler>(
           grpc::CreateChannel(GetListenAddress(), grpc::InsecureChannelCredentials()) 
        );

        offlineConsumer_ = std::make_unique<KafkaConsumer>(
            kafka_group_id + "_offline", offline_topics);
        offlineConsumer_->setHandler(offlineHandler);

        offline_thread_ = std::thread([this]() {
            offlineConsumer_->start();
        });

        spdlog::info("PushServer: Offline Kafka consumer started, group={}, topics_count={}",
                    kafka_group_id + "_offline", offline_topics.size());
    }
}

::grpc::Status PushServer::PushMsg(::grpc::ServerContext *context,
                                   const ::push::PushMsgReq *request,
                                   ::push::PushMsgResp *response) {
    // 获取发送者信息
    if (!request->has_msgdata()) {
        spdlog::error("PushServer::PushMsg: no msgdata in request");
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "msgdata is required");
    }

    const auto &msgData = request->msgdata();
    std::string conversationID = request->conversationid();

    spdlog::info("[PushService] PushMsg: sender={}, recv={}, convID={}, content_type={}",
                 msgData.sendid(), msgData.recvid(), conversationID, msgData.contenttype());

    // TODO: sender token 验证
    // 当前 PushMsg 由 msg_service (Kafka 消费链路) 内部调用，msg_service 已校验 sender
    // 后续若开放外部调用，需从此处 context metadata 提取 authorization token 并验证 sender
    spdlog::info("[PushService] PushMsg sender={} - token validation deferred (TODO)", msgData.sendid());

    // 获取接收者的在线状态和推送token（从Redis读取）
    // user:${userID}:online 存储在线状态
    // user:${userID}:push_token 存储离线推送token
    
    bool recvOnline = false;
    std::string recvPushToken;
    
    try {
        auto &redis = RedisConnector::get_instance().get_redis();
        
        // 检查接收者是否在线
        auto onlineVal = redis.get("user:" + msgData.recvid() + ":online");
        if (onlineVal) {
            recvOnline = (*onlineVal == "1" || *onlineVal == "true");
        }
            
    } catch (const std::exception &e) {
        spdlog::error("PushServer::PushMsg: Redis error: {}", e.what());
    }
    

    if (recvOnline) {
        // 接收者在线 -> 通过 gRPC 调用 GatewayPushService 推送至 WebSocket 长连接
        spdlog::info("PushServer::PushMsg: receiver {} is online, pushing via GatewayPushService gRPC", msgData.recvid());

        // 将 MsgData 序列化为 bytes（由 Gateway 负责构建 SdkWSResp）
        std::string msgDataBin = msgData.SerializeAsString();

        // 如果是群聊，需要推送给群内所有在线成员
        bool isGroupMsg = msgData.isgroupmsg();
        if (isGroupMsg) {
            // 群聊消息推送给 conversationID（群ID）的所有在线成员
            spdlog::info("PushServer::PushMsg: group message for group={}, pushing to group members",
                         conversationID);
            // TODO: 查询群成员列表后逐个推送
            // 当前简化：直接推送给 recvID（群 ID 对应的会话）
            auto& gateway = getGatewayPushClient();
            gateway.PushToUser(msgData.recvid(), msgDataBin, conversationID);
        } else {
            // 单聊消息直推给接收者
            spdlog::info("PushServer::PushMsg: direct message to user={}", msgData.recvid());
            auto& gateway = getGatewayPushClient();
            bool pushed = gateway.PushToUser(msgData.recvid(), msgDataBin, conversationID);
            if (pushed) {
                spdlog::info("PushServer::PushMsg: Gateway push to user={} succeeded", msgData.recvid());
                // 在线消息推送到Gateway后，加入pending_ack集合等待客户端ACK
                // Gateway在WebSocket下发成功后不会立即ACK，而是等待客户端回ACK
                try {
                    auto &redis = RedisConnector::get_instance().get_redis();
                    std::string ackKey = "pending_ack:online:" + msgData.servermsgid();
                    redis.sadd(ackKey, msgData.recvid());
                    spdlog::info("PushServer::PushMsg: added to online pending_ack, key={}, user={}", ackKey, msgData.recvid());
                } catch (const std::exception &e) {
                    spdlog::error("PushServer::PushMsg: failed to add pending_ack: {}", e.what());
                }
            } else {
                spdlog::warn("PushServer::PushMsg: Gateway push to user={} failed (no active session or gRPC error)", msgData.recvid());
            }
        }
    } else {
        std::string recvID = msgData.recvid();
        std::string serverMsgID = msgData.servermsgid();
        // 1. 写入 Redis 热数据（Hash: offline:{recvID} -> field=serverMsgID, value=serialized MsgData）
        if (RedisHandler::SaveOfflineMsg(recvID, msgData)) {
            spdlog::info("OfflineMsgHandler: saved to Redis hot storage, recvid={}, serverMsgID={}",
                         recvID, serverMsgID);
        } else {
            spdlog::error("OfflineMsgHandler: failed to save to Redis, recvid={}, serverMsgID={}",
                          recvID, serverMsgID);
        }

        // 2. 写入 MongoDB 冷数据（TTL 自动过期，默认 30 天）
        if (MongoHandler::SaveOfflineMsgToMongo(msgData)) {
            spdlog::info("OfflineMsgHandler: saved to MongoDB cold storage, recvid={}, serverMsgID={}",
                         recvID, serverMsgID);
        } else {
            spdlog::warn("OfflineMsgHandler: failed to save to MongoDB, recvid={}, serverMsgID={}",
                         recvID, serverMsgID);
        }
    }

    // 记录推送日志到数据库
    spdlog::info("PushServer::PushMsg: push completed for conv={}", conversationID);
    return ::grpc::Status::OK;
}

::grpc::Status PushServer::AckMsg(::grpc::ServerContext *context,
                                   const ::push::AckMsgReq *request,
                                   ::push::AckMsgResp *response) {
    std::string userID = request->userid();
    std::string serverMsgID = request->servermsgid();
    int64_t seq = request->seq();
    int32_t ackType = request->acktype();

    spdlog::info("[PushService] AckMsg: userID={}, serverMsgID={}, seq={}, ackType={}",
                 userID, serverMsgID, seq, ackType);

    try {
        auto &redis = RedisConnector::get_instance().get_redis();

        if (ackType == 1) {
            // === 在线消息ACK ===
            std::string ackKey = "pending_ack:online:" + serverMsgID;
            // 从pending_ack集合中移除该用户
            redis.srem(ackKey, userID);
            spdlog::info("[PushService] AckMsg: removed user from online pending_ack, key={}, user={}", ackKey, userID);

            // 检查该消息是否所有接收者都已ACK
            long long remaining = redis.scard(ackKey);
            if (remaining == 0) {
                // 全部ACK完成，从Redis取出消息存入MongoDB
                auto msgDataOpt = RedisHandler::GetMsgInfo(serverMsgID);
                if (msgDataOpt.has_value()) {
                    sdkws::MsgData msgData = msgDataOpt.value();
                    if (MongoHandler::SaveMsgToMongo(msgData)) {
                        spdlog::info("[PushService] AckMsg: all online ACK done, saved to Mongo, serverMsgID={}", serverMsgID);
                    } else {
                        spdlog::warn("[PushService] AckMsg: failed to save to Mongo, serverMsgID={}", serverMsgID);
                    }
                }
                // 清理pending_ack集合
                redis.del(ackKey);
            } else {
                spdlog::info("[PushService] AckMsg: online pending_ack remaining={}, serverMsgID={}", remaining, serverMsgID);
            }
        } else if (ackType == 2) {
            // === 离线消息ACK ===
            // 1. 从Redis离线存储中删除
            if (RedisHandler::AckOfflineMsg(userID, serverMsgID)) {
                spdlog::info("[PushService] AckMsg: removed offline msg from Redis, user={}, serverMsgID={}", userID, serverMsgID);
            }
            // 2. 标记MongoDB离线消息为已推送
            std::vector<std::string> msgIds = {serverMsgID};
            MongoHandler::MarkOfflineMsgsPushed(userID, msgIds);
            spdlog::info("[PushService] AckMsg: marked offline msg as pushed in Mongo, user={}, serverMsgID={}", userID, serverMsgID);
        } else {
            spdlog::warn("[PushService] AckMsg: unknown ackType={}", ackType);
        }

        // 更新用户last_seq（如果seq更大）
        std::string seqKey = "user:" + userID + ":last_seq";
        auto lastSeqOpt = redis.get(seqKey);
        int64_t currentSeq = lastSeqOpt ? std::stoll(*lastSeqOpt) : 0;
        if (seq > currentSeq) {
            redis.set(seqKey, std::to_string(seq));
        }

    } catch (const std::exception &e) {
        spdlog::error("[PushService] AckMsg: Redis error: {}", e.what());
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }

    return ::grpc::Status::OK;
}

::grpc::Status PushServer::AddPendingOfflineAck(::grpc::ServerContext *context,
                                                 const ::push::AddPendingOfflineAckReq *request,
                                                 ::push::AddPendingOfflineAckResp *response) {
    std::string userID = request->userid();
    std::string serverMsgID = request->servermsgid();
    int64_t seq = request->seq();

    spdlog::info("[PushService] AddPendingOfflineAck: userID={}, serverMsgID={}, seq={}",
                 userID, serverMsgID, seq);

    try {
        // 将离线消息添加到pending_ack集合，等待客户端ACK
        auto &redis = RedisConnector::get_instance().get_redis();
        std::string ackKey = "pending_ack:offline:" + userID + ":" + serverMsgID;
        redis.set(ackKey, "1");
        redis.expire(ackKey, 86400*30); // 24小时超时，防止遗留
        spdlog::info("[PushService] AddPendingOfflineAck: created pending_ack entry, key={}", ackKey);
    } catch (const std::exception &e) {
        spdlog::error("[PushService] AddPendingOfflineAck: Redis error: {}", e.what());
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, e.what());
    }

    return ::grpc::Status::OK;
}

GatewayPushClient& PushServer::getGatewayPushClient() {
    if (gateway_push_client_) {
        return *gateway_push_client_;
    }

    // 延迟初始化：通过 BackbonService (etcd) 服务发现获取 GatewayPushService 地址
    std::string gateway_addr = "localhost:50055";  // fallback
    auto* backbon = GetBackbonClient();
    if (backbon) {
        try {
            auto resp = backbon->GetServicesList("GatewayPushService");
            if (resp.registered() && resp.ipport_size() > 0) {
                gateway_addr = resp.ipport(0);
                spdlog::info("[PushService] discovered GatewayPushService at {}", gateway_addr);
            } else {
                spdlog::warn("[PushService] GatewayPushService not found in etcd, using fallback {}", gateway_addr);
            }
        } catch (const std::exception& e) {
            spdlog::error("[PushService] failed to discover GatewayPushService: {}, using fallback", e.what());
        }
    }

    gateway_channel_ = grpc::CreateChannel(gateway_addr,
                                           grpc::InsecureChannelCredentials());
    gateway_push_client_ = std::make_unique<GatewayPushClient>(gateway_channel_);
    return *gateway_push_client_;
}

::grpc::Status PushServer::DelUserPushToken(::grpc::ServerContext *context,
                                             const ::push::DelUserPushTokenReq *request,
                                             ::push::DelUserPushTokenResp *response) {
    // To-do

    return ::grpc::Status::OK;
}