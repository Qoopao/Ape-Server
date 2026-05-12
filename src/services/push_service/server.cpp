#include "services/push_service/server.h"
#include "messagequeue/kafkaconsumer.h"
#include "services/backbon_service/client.h"
#include "services/gateway_push_service/client.h"
#include "services/push_service/push_msg_handler.h"
#include "util/redisconnector.h"
#include "util/redishandler.h"
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
}

void PushServer::Start(const std::string &kafka_group_id,
                       const std::vector<std::string> &kafka_topics) {

    // 先启动 gRPC 服务器（继承自 BaseServiceServer）
    BaseServiceServer<PushServer>::Start();

    // 创建PushHandler，gRPC调用本服务的PushMsg
    auto pushHandler = std::make_shared<PushHandler>(
        grpc::CreateChannel(GetListenAddress(), grpc::InsecureChannelCredentials()));

    // 在独立线程中启动Kafka消费者，避免阻塞主线程和 gRPC 服务器
    kafkaConsumer_ = std::make_unique<KafkaConsumer>(
        kafka_group_id, kafka_topics);
    kafkaConsumer_->setHandler(pushHandler);

    kafka_thread_ = std::thread([this]() {
        kafkaConsumer_->start();
    });

    spdlog::info("PushServer: Kafka consumer started in background thread, group={}, topics_count={}",
                kafka_group_id, kafka_topics.size());
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
            
        // 获取接收者的推送token（用于离线推送：FCM/APNs token）
        auto tokenVal = redis.get("user:" + msgData.recvid() + ":push_token");
        if (tokenVal) {
            recvPushToken = *tokenVal;
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
            } else {
                spdlog::warn("PushServer::PushMsg: Gateway push to user={} failed (no active session or gRPC error)", msgData.recvid());
            }
        }
    } else {
        // 接收者离线 -> 走离线推送（FCM/APNs）
        if (!recvPushToken.empty()) {
            spdlog::info("PushServer::PushMsg: receiver {} is offline, sending push notification via token={}",
                         msgData.recvid(), recvPushToken);
            // TODO: 调用FCM/APNs推送接口
            spdlog::info("PushServer::PushMsg: [TODO] FCM/APNs push to token={} with msg from sender={}",
                         recvPushToken, msgData.sendid());
        } else {
            spdlog::warn("PushServer::PushMsg: receiver {} is offline and has no push token, push skipped",
                         msgData.recvid());
        }
    }

    // 记录推送日志到数据库
    spdlog::info("PushServer::PushMsg: push completed for conv={}", conversationID);
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