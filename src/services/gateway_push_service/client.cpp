#include "services/gateway_push_service/client.h"
#include "util/grpc_async_util.h"
#include "om/otel_trace_propagation.h"
#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>
#include <spdlog/spdlog.h>

GatewayPushClient::GatewayPushClient(std::shared_ptr<grpc::Channel> channel)
    : stub_(gateway_push::GatewayPushService::NewStub(channel)) {}

boost::asio::awaitable<bool> GatewayPushClient::PushToUser(const uint64_t account,
                                                           const std::string &msgDataBin,
                                                           const std::string &conversationID) {
    grpc::ClientContext ctx;
    ape::otel::InjectTraceContextToGrpcMetadata(ctx);
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));

    gateway_push::PushToUserReq req;
    req.set_account(account);
    req.set_msgdatabin(msgDataBin);
    req.set_conversationid(conversationID);

    gateway_push::PushToUserResp resp;

    auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
        stub_->async()->PushToUser(&ctx, &req, &resp,
            std::forward<decltype(handler)>(handler));
    });

    if (!status.ok()) {
        spdlog::error("GatewayPushClient::PushToUser: gRPC call failed for user={}, error={}",
                      account, status.error_message());
        co_return false;
    }

    co_return resp.success();
}