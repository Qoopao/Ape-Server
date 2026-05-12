#include "services/gateway_push_service/client.h"
#include <grpcpp/client_context.h>
#include <spdlog/spdlog.h>

GatewayPushClient::GatewayPushClient(std::shared_ptr<grpc::Channel> channel)
    : stub_(gateway_push::GatewayPushService::NewStub(channel)) {}

bool GatewayPushClient::PushToUser(const std::string &userID,
                                   const std::string &msgDataBin,
                                   const std::string &conversationID) {
    grpc::ClientContext ctx;
    // 设置超时（2秒，推消息不应等待太久）
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));

    gateway_push::PushToUserReq req;
    req.set_userid(userID);
    req.set_msgdatabin(msgDataBin);
    req.set_conversationid(conversationID);

    gateway_push::PushToUserResp resp;
    grpc::Status status = stub_->PushToUser(&ctx, req, &resp);

    if (!status.ok()) {
        spdlog::error("GatewayPushClient::PushToUser: gRPC call failed for user={}, error={}",
                      userID, status.error_message());
        return false;
    }

    return resp.success();
}