#include "services/push_service/client.h"
#include <spdlog/spdlog.h>

::push::PushMsgResp PushClient::PushMsg(const ::push::PushMsgReq& request) {
    ::push::PushMsgResp response;
    grpc::ClientContext context;

    auto status = stub_->PushMsg(&context, request, &response);
    if (!status.ok()) {
        spdlog::error("PushClient::PushMsg failed: {}", status.error_message());
        throw std::runtime_error("PushMsg gRPC failed: " + std::string(status.error_message()));
    }

    spdlog::info("PushClient::PushMsg: success, convID={}", request.conversationid());
    return response;
}

::push::DelUserPushTokenResp PushClient::DelUserPushToken(const ::push::DelUserPushTokenReq& request) {
    ::push::DelUserPushTokenResp response;
    grpc::ClientContext context;

    auto status = stub_->DelUserPushToken(&context, request, &response);
    if (!status.ok()) {
        spdlog::error("PushClient::DelUserPushToken failed: {}", status.error_message());
        throw std::runtime_error("DelUserPushToken gRPC failed: " + std::string(status.error_message()));
    }

    spdlog::info("PushClient::DelUserPushToken: success, userID={}", request.userid());
    return response;
}

::push::AckMsgResp PushClient::AckMsg(const ::push::AckMsgReq& request) {
    ::push::AckMsgResp response;
    grpc::ClientContext context;

    auto status = stub_->AckMsg(&context, request, &response);
    if (!status.ok()) {
        spdlog::error("PushClient::AckMsg failed: {}", status.error_message());
        throw std::runtime_error("AckMsg gRPC failed: " + std::string(status.error_message()));
    }

    spdlog::info("PushClient::AckMsg: success, userID={}, serverMsgID={}", request.userid(), request.servermsgid());
    return response;
}

::push::AddPendingOfflineAckResp PushClient::AddPendingOfflineAck(const ::push::AddPendingOfflineAckReq& request) {
    ::push::AddPendingOfflineAckResp response;
    grpc::ClientContext context;

    auto status = stub_->AddPendingOfflineAck(&context, request, &response);
    if (!status.ok()) {
        spdlog::error("PushClient::AddPendingOfflineAck failed: {}", status.error_message());
        throw std::runtime_error("AddPendingOfflineAck gRPC failed: " + std::string(status.error_message()));
    }

    spdlog::info("PushClient::AddPendingOfflineAck: success, userID={}, serverMsgID={}", request.userid(), request.servermsgid());
    return response;
}
