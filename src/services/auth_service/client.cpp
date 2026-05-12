#include "services/auth_service/client.h"
#include <spdlog/spdlog.h>

::auth::ValidateTokenResp AuthClient::ValidateToken(const std::string& token) {
    ::auth::ValidateTokenReq request;
    request.set_token(token);

    ::auth::ValidateTokenResp response;
    grpc::ClientContext context;

    auto status = stub_->ValidateToken(&context, request, &response);
    if (!status.ok()) {
        spdlog::error("AuthClient::ValidateToken gRPC failed: {}", status.error_message());
        response.set_valid(false);
        return response;
    }

    spdlog::info("AuthClient::ValidateToken: token={}, valid={}, user={}",
                 token, response.valid(), response.username());
    return response;
}