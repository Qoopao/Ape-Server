#include "services/auth_service/server.h"
#include "user/userinfo.h"
#include "util/mysqlhandler.h"
#include "util/redisconnector.h"
#include "util/redishandler.h"
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
// Token Redis key 前缀
// ──────────────────────────────────────────
static constexpr const char *TOKEN_KEY_PREFIX = "token:";

// Token 有效期（秒）
static constexpr int64_t TOKEN_TTL_SECONDS = 7 * 24 * 3600; // 7 天

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
    unsigned int hash_len = 0;
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(mdctx, input.data(), input.size());
    EVP_DigestFinal_ex(mdctx, hash, &hash_len);
    EVP_MD_CTX_free(mdctx);

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
// Token 持久化到 Redis
// Key:   token:{token}
// Value: {user_id},{username}
// TTL:   7 天（与 expires_at 对齐）
// ──────────────────────────────────────────
void AuthServiceImpl::persist_token(const std::string &user_id,
                                    const std::string &username,
                                    const std::string &token,
                                    int64_t expires_at) {
    try {
        auto &redis = RedisConnector::get_instance().get_redis();
        std::string key = TOKEN_KEY_PREFIX + token;
        std::string value = user_id + "," + username;
        redis.setex(key, TOKEN_TTL_SECONDS, value);
        spdlog::info("[AuthService] Token persisted to Redis: user={}, token={}",
                     user_id, token);
    } catch (const std::exception &e) {
        spdlog::error("[AuthService] Failed to persist token to Redis: user={}, error={}",
                      user_id, e.what());
    }
}

// ──────────────────────────────────────────
// 注册接口
// ──────────────────────────────────────────
::grpc::Status AuthServiceImpl::Register(::grpc::ServerContext *context,
                                         const ::auth::RegisterRequest *request,
                                         ::auth::AuthResponse *response) {
    const std::string &username = request->username();
    const std::string &password = request->password();
    const std::string &nickname = request->nickname();

    spdlog::info("[AuthService] Register request: username={}", username);

    // 参数校验
    if (username.empty() || password.empty()) {
        response->set_success(false);
        response->set_error_code("INVALID_PARAM");
        response->set_error_message("Username and password are required");
        spdlog::warn("[AuthService] Register failed: empty username or password");
        return ::grpc::Status::OK;
    }

    // 检查用户名是否已存在
    if (MySQLHandler::username_exists(username)) {
        response->set_success(false);
        response->set_error_code("USERNAME_EXISTS");
        response->set_error_message("Username already exists");
        spdlog::warn("[AuthService] Register failed: username {} already exists", username);
        return ::grpc::Status::OK;
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
        spdlog::error("[AuthService] Register failed: DB insert error for username={}", username);
        return ::grpc::Status::OK;
    }

    // 生成 token 并持久化
    std::string token = generate_token();
    int64_t expires_at = calculate_expiry();
    persist_token(user.user_id, user.username, token, expires_at);

    // 构造响应
    response->set_success(true);
    response->set_token(token);
    response->set_expires_at(expires_at);

    auto *auth_user = response->mutable_user();
    auth_user->set_user_id(user.user_id);
    auth_user->set_username(user.username);
    auth_user->set_nickname(user.nickname);

    spdlog::info("[AuthService] User registered: {} ({})", username,
                 user.user_id);

    return ::grpc::Status::OK;
}

// ──────────────────────────────────────────
// 登录接口
// ──────────────────────────────────────────
::grpc::Status AuthServiceImpl::Login(::grpc::ServerContext *context,
                                      const ::auth::LoginRequest *request,
                                      ::auth::AuthResponse *response) {
    const std::string &username = request->username();
    const std::string &password = request->password();

    spdlog::info("[AuthService] Login request: username={}", username);

    // 参数校验
    if (username.empty() || password.empty()) {
        response->set_success(false);
        response->set_error_code("INVALID_PARAM");
        response->set_error_message("Username and password are required");
        spdlog::warn("[AuthService] Login failed: empty username or password");
        return ::grpc::Status::OK;
    }

    // 查询用户
    auto user_opt = MySQLHandler::find_user_by_username(username);
    if (!user_opt.has_value()) {
        response->set_success(false);
        response->set_error_code("USER_NOT_FOUND");
        response->set_error_message("Invalid username or password");
        spdlog::warn("[AuthService] Login failed: username {} not found", username);
        return ::grpc::Status::OK;
    }

    const auto &user = user_opt.value();

    // 验证密码
    std::string input_hash = hash_password(password, user.password_salt);
    if (input_hash != user.password_hash) {
        response->set_success(false);
        response->set_error_code("WRONG_PASSWORD");
        response->set_error_message("Invalid username or password");
        spdlog::warn("[AuthService] Login failed: wrong password for username={}", username);
        return ::grpc::Status::OK;
    }

    // 更新最后登录时间
    MySQLHandler::update_last_login(user.user_id);

    // 生成新 token 并持久化（覆盖旧 token）
    std::string token = generate_token();
    int64_t expires_at = calculate_expiry();
    persist_token(user.user_id, user.username, token, expires_at);

    // 构造响应
    response->set_success(true);
    response->set_token(token);
    response->set_expires_at(expires_at);

    auto *auth_user = response->mutable_user();
    auth_user->set_user_id(user.user_id);
    auth_user->set_username(user.username);
    auth_user->set_nickname(user.nickname);

    spdlog::info("[AuthService] User logged in: {} ({})", username,
                 user.user_id);

    return ::grpc::Status::OK;
}

// ──────────────────────────────────────────
// Token 验证接口
// 1. 查询 Redis: GET token:{token}
// 2. 命中 → 解析 user_id,username → 返回 valid=true
// 3. 未命中 → 返回 valid=false
// ──────────────────────────────────────────
::grpc::Status AuthServiceImpl::ValidateToken(
    ::grpc::ServerContext *context,
    const ::auth::ValidateTokenReq *request,
    ::auth::ValidateTokenResp *response) {

    const std::string &token = request->token();

    spdlog::info("[AuthService] ValidateToken request: token={}", token);

    if (token.empty()) {
        response->set_valid(false);
        spdlog::warn("[AuthService] ValidateToken failed: empty token");
        return ::grpc::Status::OK;
    }

    try {
        auto &redis = RedisConnector::get_instance().get_redis();
        std::string key = TOKEN_KEY_PREFIX + token;
        auto value_opt = redis.get(key);

        if (value_opt) {
            // Redis 命中，解析 user_id,username
            const std::string &value = *value_opt;
            auto comma_pos = value.find(',');
            if (comma_pos != std::string::npos) {
                std::string user_id = value.substr(0, comma_pos);
                std::string username = value.substr(comma_pos + 1);

                // 获取 TTL 作为 expires_at
                auto ttl = redis.ttl(key);

                response->set_valid(true);
                response->set_user_id(user_id);
                response->set_username(username);
                if (ttl >= 0) {
                    // 计算绝对过期时间 = 当前时间 + TTL 秒
                    auto now = std::chrono::system_clock::now();
                    auto expires_at = std::chrono::duration_cast<std::chrono::seconds>(
                        (now + std::chrono::seconds(ttl)).time_since_epoch()).count();
                    response->set_expires_at(expires_at);
                }

                spdlog::info("[AuthService] ValidateToken success: user={}, username={}",
                             user_id, username);
                return ::grpc::Status::OK;
            }
        }

        // Redis 未命中
        response->set_valid(false);
        spdlog::warn("[AuthService] ValidateToken failed: token not found or expired");
        return ::grpc::Status::OK;

    } catch (const std::exception &e) {
        spdlog::error("[AuthService] ValidateToken Redis error: {}", e.what());
        response->set_valid(false);
        return ::grpc::Status(::grpc::StatusCode::INTERNAL, "Redis error");
    }
}