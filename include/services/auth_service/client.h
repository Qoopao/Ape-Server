#ifndef AUTH_SERVICE_CLIENT_H
#define AUTH_SERVICE_CLIENT_H

#include "apeauth.grpc.pb.h"
#include "apeauth.pb.h"
#include <cstdint>
#include <grpcpp/channel.h>
#include <boost/asio/awaitable.hpp>
#include <memory>
#include <string>

class AuthClient {
public:
    AuthClient(std::shared_ptr<grpc::Channel> channel)
        : stub_(auth::AuthService::NewStub(channel)) {}

    boost::asio::awaitable<::auth::ValidateTokenResponse> ValidateToken(const std::string& token);
    boost::asio::awaitable<::auth::AuthRegResponse> LoginRequest(const uint64_t account, const std::string& plainPWD);
    boost::asio::awaitable<::auth::AuthRegResponse> RegisterRequest(const std::string& phone, const std::string& nickname, const std::string& password, const std::string& requestId);

private:
    std::unique_ptr<auth::AuthService::Stub> stub_;
};

#endif