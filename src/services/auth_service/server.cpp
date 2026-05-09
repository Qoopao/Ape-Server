#include "services/auth_service/server.h"
#include "user/userinfo.h"
#include "util/mysqlhandler.h"
#include "util/uuid.h"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>

// ──────────────────────────────────────────
// 构造
// ──────────────────────────────────────────
AuthServiceImpl::AuthServiceImpl(const std::string &service_name,
                                 const std::string &listen_address)
    : BaseServiceServer<AuthServiceImpl>(service_name, listen_address) {}

// ──────────────────────────────────────────
// 密码哈希: SHA256(salt + password)
// ──────────────────────────────────────────
std::string AuthServiceImpl::hash_password(const std::string &password,
                                           const std::string &salt) {
    std::string input = salt + password;

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, input.data(), input.size());
    SHA256_Final(hash, &sha256);

    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<int>(hash[i]);
    }
    return oss.str();
}

// ──────────────────────────────────────────
// 生成随机盐值 (16 bytes -> 32 hex chars)
// ──────────────────────────────────────────
std::string AuthServiceImpl::generate_salt() {
    // 使用 boost::uuid 作为 16 字节的随机盐
    return boost::uuids::to_string(boost::uuids::random_generator()()).substr(
        0, 32);
}

// ──────────────────────────────────────────
// 生成 token
// ──────────────────────────────────────────
std::string AuthServiceImpl::generate_token() {
    return boost::uuids::to_string(boost::uuids::random_generator()());
}

// ──────────────────────────────────────────
// 过期时间: 当前 unix 时间 + 7 天
// ──────────────────────────────────────────
int64_t AuthServiceImpl::calculate_expiry() {
    auto now = std::chrono::system_clock::now();
    auto seven_days = now + std::chrono::hours(24 * 7);
    return std::chrono::duration_cast<std::chrono::seconds>(
               seven_days.time_since_epoch())
        .count();
}

// ──────────────────────────────────────────
// 注册接口
// ──────────────────────────────────────────
::grpc::ServerUnaryReactor *
AuthServiceImpl::Register(::grpc::CallbackServerContext *context,
                          const ::auth::RegisterRequest *request,
                          ::auth::AuthResponse *response) {
    auto *reactor = context->DefaultReactor();

    const std::string &username = request->username();
    const std::string &password = request->password();
    const std::string &nickname = request->nickname();

    spdlog::info("[AuthService] Register request: username={}", username);

    // 参数校验
    if (username.empty() || password.empty()) {
        response->set_success(false);
        response->set_error_code("INVALID_PARAM");
        response->set_error_message("Username and password are required");
        reactor->Finish(::grpc::Status::OK);
        return reactor;
    }

    // 检查用户名是否已存在
    if (MySQLHandler::username_exists(username)) {
        response->set_success(false);
        response->set_error_code("USERNAME_EXISTS");
        response->set_error_message("Username already exists");
        reactor->Finish(::grpc::Status::OK);
        return reactor;
    }

    // 创建用户
    userInfo user;
    user.user_id = uuid::newone_str();
    user.username = username;
    user.nickname = nickname.empty() ? username : nickname;
    user.password_salt = generate_salt();
    user.password_hash = hash_password(password, user.password_salt);

    auto result = MySQLHandler::insert_user(user);
    if (!result.has_value()) {
        response->set_success(false);
        response->set_error_code("DB_ERROR");
        response->set_error_message("Failed to create user in database");
        reactor->Finish(::grpc::Status::OK);
        return reactor;
    }

    // 构造响应
    response->set_success(true);
    response->set_token(generate_token());
    response->set_expires_at(calculate_expiry());

    auto *auth_user = response->mutable_user();
    auth_user->set_user_id(user.user_id);
    auth_user->set_username(user.username);
    auth_user->set_nickname(user.nickname);

    spdlog::info("[AuthService] User registered: {} ({})", username,
                 user.user_id);

    reactor->Finish(::grpc::Status::OK);
    return reactor;
}

// ──────────────────────────────────────────
// 登录接口
// ──────────────────────────────────────────
::grpc::ServerUnaryReactor *
AuthServiceImpl::Login(::grpc::CallbackServerContext *context,
                       const ::auth::LoginRequest *request,
                       ::auth::AuthResponse *response) {
    auto *reactor = context->DefaultReactor();

    const std::string &username = request->username();
    const std::string &password = request->password();

    spdlog::info("[AuthService] Login request: username={}", username);

    // 参数校验
    if (username.empty() || password.empty()) {
        response->set_success(false);
        response->set_error_code("INVALID_PARAM");
        response->set_error_message("Username and password are required");
        reactor->Finish(::grpc::Status::OK);
        return reactor;
    }

    // 查询用户
    auto user_opt = MySQLHandler::find_user_by_username(username);
    if (!user_opt.has_value()) {
        response->set_success(false);
        response->set_error_code("USER_NOT_FOUND");
        response->set_error_message("Invalid username or password");
        reactor->Finish(::grpc::Status::OK);
        return reactor;
    }

    const auto &user = user_opt.value();

    // 验证密码
    std::string input_hash = hash_password(password, user.password_salt);
    if (input_hash != user.password_hash) {
        response->set_success(false);
        response->set_error_code("WRONG_PASSWORD");
        response->set_error_message("Invalid username or password");
        reactor->Finish(::grpc::Status::OK);
        return reactor;
    }

    // 更新最后登录时间
    MySQLHandler::update_last_login(user.user_id);

    // 构造响应
    response->set_success(true);
    response->set_token(generate_token());
    response->set_expires_at(calculate_expiry());

    auto *auth_user = response->mutable_user();
    auth_user->set_user_id(user.user_id);
    auth_user->set_username(user.username);
    auth_user->set_nickname(user.nickname);

    spdlog::info("[AuthService] User logged in: {} ({})", username,
                 user.user_id);

    reactor->Finish(::grpc::Status::OK);
    return reactor;
}