#include "services/gateway_push_service/server.h"
#include "gateway/ws_session_manager.h"
#include "sdkws.pb.h"
#include <spdlog/spdlog.h>

GatewayPushServer::GatewayPushServer(const std::string &service_name,
                                     const std::string &listen_address)
    : BaseServiceServer<GatewayPushServer>(service_name, listen_address) {}

::grpc::Status GatewayPushServer::PushToUser(
    ::grpc::ServerContext *context,
    const ::gateway_push::PushToUserReq *request,
    ::gateway_push::PushToUserResp *response) {

    const std::string &userID = request->userid();
    const std::string &msgDataBin = request->msgdatabin();

    if (userID.empty()) {
        spdlog::error("GatewayPushServer::PushToUser: empty userID");
        return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                              "userID is required");
    }

    spdlog::info("GatewayPushServer::PushToUser: userID={}, dataLen={}",
                 userID, msgDataBin.size());

    // 构建 WebSocket 推送消息（SdkWSResp, type=104）
    sdkws::SdkWSResp pushResp;
    pushResp.set_type(104);  // 104: 下推用户消息
    pushResp.set_userid(userID);
    pushResp.set_data(msgDataBin);

    std::string pushBin = pushResp.SerializeAsString();
    auto payload = std::make_shared<std::string>(std::move(pushBin));

    bool pushed = WSSessionManager::instance().pushToUser(userID, payload);

    response->set_success(pushed);
    if (pushed) {
        spdlog::info("GatewayPushServer::PushToUser: pushed to user={} succeeded",
                     userID);
    } else {
        spdlog::warn("GatewayPushServer::PushToUser: push to user={} failed (no active session)",
                     userID);
    }

    return ::grpc::Status::OK;
}