#ifndef AUTH_SERVICE_SERVER_H
#define AUTH_SERVICE_SERVER_H

#include "auth.grpc.pb.h"
#include "auth.pb.h"
#include "services/base_service.h"

class AuthServiceImpl final
    : public auth::AuthService::CallbackService,
      public BaseServiceServer<AuthServiceImpl> {
public:
    AuthServiceImpl(const std::string &service_name,
                    const std::string &listen_address);
    ~AuthServiceImpl() override = default;

    // 用户注册
    ::grpc::ServerUnaryReactor *
    Register(::grpc::CallbackServerContext *context,
             const ::auth::RegisterRequest *request,
             ::auth::AuthResponse *response) override;

    // 用户登录
    ::grpc::ServerUnaryReactor *
    Login(::grpc::CallbackServerContext *context,
          const ::auth::LoginRequest *request,
          ::auth::AuthResponse *response) override;

private:
    // 生成密码哈希: SHA256(salt + password)
    static std::string hash_password(const std::string &password,
                                     const std::string &salt);
    // 生成随机盐值 (32 hex chars = 16 bytes)
    static std::string generate_salt();
    // 生成 token
    static std::string generate_token();
    // 计算过期时间 (当前时间 + 7 天)
    static int64_t calculate_expiry();
};

#endif