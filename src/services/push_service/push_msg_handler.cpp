#include "services/push_service/push_msg_handler.h"
#include "sdkws.pb.h"
#include <spdlog/spdlog.h>

PushHandler::PushHandler(std::shared_ptr<grpc::Channel> pushChannel)
    : pushStub_(push::PushService::NewStub(pushChannel)) {}

void PushHandler::handle(const std::string& topic, const std::string& msg) {
    // 尝试解析为PushMsgReq（已经包含MsgData + conversationID）
    push::PushMsgReq req;
    if (req.ParseFromString(msg)) {
        push::PushMsgResp resp;
        grpc::ClientContext ctx;
        auto status = pushStub_->PushMsg(&ctx, req, &resp);
        if (!status.ok()) {
            spdlog::error("PushHandler: gRPC PushMsg failed: {}", status.error_message());
        } else {
            spdlog::info("PushHandler: PushMsg success via PushMsgReq, topic={}", topic);
        }
        return;
    }

    // 尝试解析为MsgData，自动构造PushMsgReq
    sdkws::MsgData msgData;
    if (msgData.ParseFromString(msg)) {
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
            spdlog::error("PushHandler: gRPC PushMsg failed: {}", status.error_message());
        } else {
            spdlog::info("PushHandler: PushMsg success via MsgData, topic={}", topic);
        }
        return;
    }

    spdlog::warn("PushHandler: Failed to parse Kafka message, topic={}, payload_size={}", topic, msg.size());
}
