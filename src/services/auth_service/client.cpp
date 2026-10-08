#include "services/auth_service/client.h"
#include "apeauth.pb.h"
#include "util/grpc_async_util.h"
#include <spdlog/spdlog.h>
#include <chrono>

boost::asio::awaitable<::auth::ValidateTokenResponse> AuthClient::ValidateToken(const std::string& token) {
    ::auth::ValidateTokenReq request;
    request.set_token(token);

    ::auth::ValidateTokenResponse response;
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
                 token, response.valid(), response.account());
    co_return response;
}

boost::asio::awaitable<::auth::AuthRegResponse> AuthClient::LoginRequest(const uint64_t account, const std::string& plainPWD) {
    ::auth::LoginRequest request;
    request.set_account(account);
    request.set_password(plainPWD);

    ::auth::AuthRegResponse response;
    grpc::ClientContext context;

    auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
        stub_->async()->Login(&context, &request, &response,
            std::forward<decltype(handler)>(handler));
    });

    if (!status.ok()) {
        spdlog::error("AuthClient::Login gRPC failed: {}", status.error_message());
        co_return response;
    }

    spdlog::info("AuthClient::Login: account={} {}, reason: {}",response.user().account(),response.success(),response.errormessage());
    co_return response;
}

boost::asio::awaitable<::auth::AuthRegResponse> AuthClient::RegisterRequest(const std::string& phone, const std::string& nickname, const std::string& password, const std::string& requestId){
    ::auth::RegisterRequest request;
    request.set_phone(phone);
    request.set_nickname(nickname);
    request.set_password(password);

    ::auth::AuthRegResponse response;
    grpc::ClientContext context;
    context.AddMetadata("x-registration-id", requestId);
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(15));

    auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
        stub_->async()->Register(&context, &request, &response,
            std::forward<decltype(handler)>(handler));
    });

    if (!status.ok()) {
        spdlog::error("AuthClient::Register gRPC failed: {}", status.error_message());
        response.set_success(false);
        response.set_errorcode("REGISTRATION_UNCERTAIN");
        response.set_errormessage("Registration outcome unknown; retry the same request ID");
        co_return response;
    }

    if(response.success()){
        spdlog::info("AuthClient::Register: account = {} , nickname = {} successfully register.", response.user().account(), nickname);
    }else{
        spdlog::info("AuthClient::Register: phone = {} failed to register, reason = {}", request.phone(), response.errormessage());
    }
    co_return response;
}