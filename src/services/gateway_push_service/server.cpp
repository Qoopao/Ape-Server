#include "services/gateway_push_service/server.h"
#include "gateway/ws_session_manager.h"
#include "sdkws.pb.h"
#include <cstdint>
#include <spdlog/spdlog.h>

GatewayPushServer::GatewayPushServer(const std::string &service_name,
                                     const std::string &listen_address)
    : BaseServiceServer<GatewayPushServer>(service_name, listen_address) {}

::grpc::ServerUnaryReactor *GatewayPushServer::PushToUser(
    ::grpc::CallbackServerContext *context,
    const ::gateway_push::PushToUserReq *request,
    ::gateway_push::PushToUserResp *response) {

    grpc::ServerUnaryReactor *reactor = context->DefaultReactor();

    const uint64_t &account = request->account();
    const std::string &msgDataBin = request->msgdatabin();

    if (account == 0) {
        spdlog::error("GatewayPushServer::PushToUser: empty account");
        response->set_success(false);
        reactor->Finish(::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                              "account is required"));
        return reactor;
    }

    spdlog::info("GatewayPushServer::PushToUser: account={}, dataLen={}",
                 account, msgDataBin.size());

    // 构建 WebSocket 推送消息（SdkWSResp, type=104）
    sdkws::SdkWSResp pushResp;
    pushResp.set_type(104);  // 104: 下推用户消息
    pushResp.set_account(account);
    pushResp.set_data(msgDataBin);

    std::string pushBin = pushResp.SerializeAsString();
    auto payload = std::make_shared<std::string>(std::move(pushBin));

    bool pushed = WSSessionManager::instance().pushToUser(account, payload);

    response->set_success(pushed);
    if (pushed) {
        spdlog::info("GatewayPushServer::PushToUser: pushed to user={} succeeded",
                     account);
    } else {
        spdlog::warn("GatewayPushServer::PushToUser: push to user={} failed (no active session)",
                     account);
    }

    reactor->Finish(::grpc::Status::OK);
    return reactor;
}