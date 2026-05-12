#include "services/push_service/push_msg_handler.h"
#include "sdkws.pb.h"
#include "util/redishandler.h"
#include "util/mongohandler.h"
#include <spdlog/spdlog.h>

PushHandler::PushHandler(std::shared_ptr<grpc::Channel> pushChannel)
    : pushStub_(push::PushService::NewStub(pushChannel)) {}

void PushHandler::handle(const std::string& topic, const std::string& msg) {
    // Kafka消息体是serverMsgID（UUID字符串），从Redis取出完整消息
    std::string serverMsgID = msg;
    spdlog::info("PushHandler: received msgid from Kafka, topic={}, serverMsgID={}", topic, serverMsgID);

    // 1. 从Redis获取完整消息数据
    auto msgDataOpt = RedisHandler::GetMsgInfo(serverMsgID);
    if (!msgDataOpt.has_value()) {
        spdlog::error("PushHandler: GetMsgInfo from Redis failed, serverMsgID={}", serverMsgID);
        return;
    }
    sdkws::MsgData msgData = msgDataOpt.value();
    spdlog::info("PushHandler: got MsgData from Redis, serverMsgID={}, sendID={}, recvID={}",
                 serverMsgID, msgData.sendid(), msgData.recvid());

    // 2. 构造PushMsgReq并调用PushService
    push::PushMsgReq req;
    *req.mutable_msgdata() = msgData;
    if (!msgData.convid().empty()) {
        req.set_conversationid(msgData.convid());
    } else {
        // 单聊时用sendID_recvID作为会话ID
        req.set_conversationid(msgData.sendid() + "_" + msgData.recvid());
    }

    push::PushMsgResp resp;
    grpc::ClientContext ctx;
    auto status = pushStub_->PushMsg(&ctx, req, &resp);
    if (!status.ok()) {
        spdlog::error("PushHandler: gRPC PushMsg failed: {}, serverMsgID={}", status.error_message(), serverMsgID);
    } else {
        spdlog::info("PushHandler: PushMsg success, serverMsgID={}", serverMsgID);
        // 3. 推送成功后，将消息持久化到MongoDB
        if (!MongoHandler::SaveMsgToMongo(msgData)) {
            spdlog::warn("PushHandler: SaveMsgToMongo failed, serverMsgID={}", serverMsgID);
        }
    }

}