#ifndef AUTH_SERVICE_CLIENT_H
#define AUTH_SERVICE_CLIENT_H

#include "apeauth.grpc.pb.h"
#include "apeauth.pb.h"
#include <grpcpp/channel.h>
#include <memory>
#include <string>

class AuthClient {
public:
    AuthClient(std::shared_ptr<grpc::Channel> channel)
        : stub_(auth::AuthService::NewStub(channel)) {}

    // Token 验证：返回 (valid, user_id, username, expires_at)
    ::auth::ValidateTokenResp ValidateToken(const std::string& token);

private:
    std::unique_ptr<auth::AuthService::Stub> stub_;
};

#endif