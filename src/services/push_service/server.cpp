#include "services/push_service/server.h"
#include "gateway/ws_session_manager.h"
#include "messagequeue/kafkaconsumer.h"
#include "services/push_service/push_msg_handler.h"
#include "util/redisconnector.h"
#include "util/redishandler.h"
#include <google/protobuf/util/json_util.h>
#include <nlohmann/json.hpp>
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
        // 接收者在线 -> 通过WebSocket长连接直接推送
        spdlog::info("PushServer::PushMsg: receiver {} is online, pushing via WebSocket", msgData.recvid());

        // 构建 WebSocket 推送消息（使用 sdkws::SdkWSResp 格式，与客户端协议一致）
        nlohmann::json pushMsg;
        pushMsg["type"] = "push";
        pushMsg["conversationID"] = conversationID;

        // 序列化 msgData 为 JSON
        google::protobuf::util::JsonPrintOptions printOpts;
        printOpts.preserve_proto_field_names = true;
        std::string msgDataJson;
        auto status = google::protobuf::util::MessageToJsonString(msgData, &msgDataJson, printOpts);
        if (!status.ok()) {
            spdlog::error("PushServer::PushMsg: failed to serialize msgData to JSON: {}", status.message());
            return ::grpc::Status(::grpc::StatusCode::INTERNAL, "failed to serialize msgData");
        }
        pushMsg["msgData"] = nlohmann::json::parse(msgDataJson);

        // 如果是群聊，需要推送给群内所有在线成员
        bool isGroupMsg = msgData.isgroupmsg();
        if (isGroupMsg) {
            // 群聊消息推送给 conversationID（群ID）的所有在线成员
            spdlog::info("PushServer::PushMsg: group message for group={}, pushing to group members",
                         conversationID);
            // TODO: 查询群成员列表后逐个推送
            // 当前简化：直接推送给 recvID（群 ID 对应的会话）
            auto payload = std::make_shared<std::string>(pushMsg.dump());
            WSSessionManager::instance().pushToUser(msgData.recvid(), payload);
        } else {
            // 单聊消息直推给接收者
            spdlog::info("PushServer::PushMsg: direct message to user={}", msgData.recvid());
            auto payload = std::make_shared<std::string>(pushMsg.dump());
            bool pushed = WSSessionManager::instance().pushToUser(msgData.recvid(), payload);
            if (pushed) {
                spdlog::info("PushServer::PushMsg: WebSocket push to user={} succeeded", msgData.recvid());
            } else {
                spdlog::warn("PushServer::PushMsg: WebSocket push to user={} failed (no active session)", msgData.recvid());
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

::grpc::Status PushServer::DelUserPushToken(::grpc::ServerContext *context,
                                             const ::push::DelUserPushTokenReq *request,
                                             ::push::DelUserPushTokenResp *response) {
    // To-do

    return ::grpc::Status::OK;
}