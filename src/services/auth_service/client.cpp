#include "services/auth_service/client.h"
#include "util/grpc_async_util.h"
#include <spdlog/spdlog.h>

boost::asio::awaitable<::auth::ValidateTokenResp> AuthClient::ValidateToken(const std::string& token) {
    ::auth::ValidateTokenReq request;
    request.set_token(token);

    ::auth::ValidateTokenResp response;
    grpc::ClientContext context;

    auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
        stub_->async()->ValidateToken(&context, &request, &response,
            std::forward<decltype(handler)>(handler));
    });

    if (!status.ok()) {
        spdlog::error("AuthClient::ValidateToken gRPC failed: {}", status.error_message());
        response.set_valid(false);
        co_return response;
    }

    spdlog::info("AuthClient::ValidateToken: token={}, valid={}, user={}",
                 token, response.valid(), response.username());
    co_return response;
}