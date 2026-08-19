#include "services/auth_service/server.h"
#include "user/userinfo.h"
#include "storage/mysqlconnector.h"
#include "storage/mysqlhandler.h"
#include "storage/redisconnector.h"
#include "util/uuid.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
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

AuthServiceImpl::AuthServiceImpl(const std::string &service_name,
                                 const std::string &listen_address)
    : BaseServiceServer<AuthServiceImpl>(service_name, listen_address) {}

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

std::string AuthServiceImpl::generate_salt() {
    return boost::uuids::to_string(boost::uuids::random_generator()()).substr(
        0, 32);
}

std::string AuthServiceImpl::generate_token() {
    return boost::uuids::to_string(boost::uuids::random_generator()());
}

int64_t AuthServiceImpl::calculate_expiry() {
    auto now = std::chrono::system_clock::now();
    auto seven_days = now + std::chrono::hours(24 * 7);
    return std::chrono::duration_cast<std::chrono::seconds>(
               seven_days.time_since_epoch())
        .count();
}

// ──────────────────────────────────────────
// Register — 全异步：用户名检查 + 写入 MySQL + Redis token 均在协程内执行
// ──────────────────────────────────────────
::grpc::ServerUnaryReactor *AuthServiceImpl::Register(
    ::grpc::CallbackServerContext *context,
    const ::auth::RegisterRequest *request,
    ::auth::AuthResponse *response) {

    grpc::ServerUnaryReactor *reactor = context->DefaultReactor();

    const std::string username = request->username();
    const std::string password = request->password();
    const std::string nickname = request->nickname();

    spdlog::info("[AuthService] Register request: username={}", username);

    // 参数校验（同步检查，无 IO）
    if (username.empty() || password.empty()) {
        response->set_success(false);
        response->set_error_code("INVALID_PARAM");
        response->set_error_message("Username and password are required");
        spdlog::warn("[AuthService] Register failed: empty username or password");
        reactor->Finish(::grpc::Status::OK);
        return reactor;
    }

    // 准备用户数据
    userInfo user;
    user.user_id = uuid::newone_str();
    user.username = username;
    user.nickname = nickname.empty() ? username : nickname;
    user.password_salt = generate_salt();
    user.password_hash = hash_password(password, user.password_salt);

    std::string token = generate_token();
    int64_t expires_at = calculate_expiry();

    // 全异步执行：MySQL 检查 + 插入 + Redis 写 token
    boost::asio::co_spawn(
        MySQLConnector::instance().get_executor(),
        [reactor, response, user = std::move(user), token, expires_at]()
            -> boost::asio::awaitable<void> {
            try {
                // ── 检查用户名是否已存在 ──
                bool exists = co_await MySQLHandler::username_exists(user.username);
                if (exists) {
                    response->set_success(false);
                    response->set_error_code("USERNAME_EXISTS");
                    response->set_error_message("Username already exists");
                    spdlog::warn(
                        "[AuthService] Register failed: username {} already exists",
                        user.username);
                    reactor->Finish(::grpc::Status::OK);
                    co_return;
                }

                // ── 插入用户到 MySQL ──
                auto result = co_await MySQLHandler::insert_user(user);
                if (!result.has_value()) {
                    response->set_success(false);
                    response->set_error_code("DB_ERROR");
                    response->set_error_message("Failed to create user in database");
                    spdlog::error(
                        "[AuthService] Register failed: DB insert error for username={}",
                        user.username);
                    reactor->Finish(::grpc::Status::OK);
                    co_return;
                }

                // ── 持久化 token 到 Redis ──
                std::string key = std::string(TOKEN_KEY_PREFIX) + token;
                std::string value = user.user_id + "," + user.username;
                co_await RedisConnector::instance().setex(key, TOKEN_TTL_SECONDS, value);
                spdlog::info(
                    "[AuthService] Token persisted to Redis: user={}, token={}",
                    user.user_id, token);

                // ── 构造响应 ──
                response->set_success(true);
                response->set_token(token);
                response->set_expires_at(expires_at);
                auto *auth_user = response->mutable_user();
                auth_user->set_user_id(user.user_id);
                auth_user->set_username(user.username);
                auth_user->set_nickname(user.nickname);

                spdlog::info("[AuthService] User registered: {} ({})",
                             user.username, user.user_id);
                reactor->Finish(::grpc::Status::OK);

            } catch (const std::exception &e) {
                spdlog::error("[AuthService] Register coroutine exception: {}", e.what());
                response->set_success(false);
                response->set_error_code("INTERNAL_ERROR");
                response->set_error_message(e.what());
                reactor->Finish(::grpc::Status(::grpc::StatusCode::INTERNAL,
                                               e.what()));
            }
        },
        boost::asio::detached);

    return reactor;
}

// ──────────────────────────────────────────
// Login — 全异步：MySQL 查询 + 密码验证 + 更新登录时间 + Redis token
// ──────────────────────────────────────────
::grpc::ServerUnaryReactor *AuthServiceImpl::Login(
    ::grpc::CallbackServerContext *context,
    const ::auth::LoginRequest *request,
    ::auth::AuthResponse *response) {

    grpc::ServerUnaryReactor *reactor = context->DefaultReactor();

    const std::string username = request->username();
    const std::string password = request->password();

    spdlog::info("[AuthService] Login request: username={}", username);

    if (username.empty() || password.empty()) {
        response->set_success(false);
        response->set_error_code("INVALID_PARAM");
        response->set_error_message("Username and password are required");
        spdlog::warn("[AuthService] Login failed: empty username or password");
        reactor->Finish(::grpc::Status::OK);
        return reactor;
    }

    std::string token = generate_token();
    int64_t expires_at = calculate_expiry();

    boost::asio::co_spawn(
        MySQLConnector::instance().get_executor(),
        [reactor, response, username, password, token, expires_at, this]()
            -> boost::asio::awaitable<void> {
            try {
                // ── 查询用户 ──
                auto user_opt = co_await MySQLHandler::find_user_by_username(username);
                if (!user_opt.has_value()) {
                    response->set_success(false);
                    response->set_error_code("USER_NOT_FOUND");
                    response->set_error_message("Invalid username or password");
                    spdlog::warn("[AuthService] Login failed: username {} not found",
                                 username);
                    reactor->Finish(::grpc::Status::OK);
                    co_return;
                }

                const auto &user = user_opt.value();

                // ── 验证密码 ──
                std::string input_hash = hash_password(password, user.password_salt);
                if (input_hash != user.password_hash) {
                    response->set_success(false);
                    response->set_error_code("WRONG_PASSWORD");
                    response->set_error_message("Invalid username or password");
                    spdlog::warn(
                        "[AuthService] Login failed: wrong password for username={}",
                        username);
                    reactor->Finish(::grpc::Status::OK);
                    co_return;
                }

                // ── 更新最后登录时间 ──
                co_await MySQLHandler::update_last_login(user.user_id);

                // ── 持久化 token 到 Redis ──
                std::string key = std::string(TOKEN_KEY_PREFIX) + token;
                std::string value = user.user_id + "," + user.username;
                co_await RedisConnector::instance().setex(key, TOKEN_TTL_SECONDS, value);
                spdlog::info(
                    "[AuthService] Token persisted to Redis: user={}, token={}",
                    user.user_id, token);

                // ── 构造响应 ──
                response->set_success(true);
                response->set_token(token);
                response->set_expires_at(expires_at);
                auto *auth_user = response->mutable_user();
                auth_user->set_user_id(user.user_id);
                auth_user->set_username(user.username);
                auth_user->set_nickname(user.nickname);

                spdlog::info("[AuthService] User logged in: {} ({})",
                             user.username, user.user_id);
                reactor->Finish(::grpc::Status::OK);

            } catch (const std::exception &e) {
                spdlog::error("[AuthService] Login coroutine exception: {}", e.what());
                response->set_success(false);
                response->set_error_code("INTERNAL_ERROR");
                response->set_error_message(e.what());
                reactor->Finish(::grpc::Status(::grpc::StatusCode::INTERNAL,
                                               e.what()));
            }
        },
        boost::asio::detached);

    return reactor;
}

::grpc::ServerUnaryReactor *AuthServiceImpl::ValidateToken(
    ::grpc::CallbackServerContext *context,
    const ::auth::ValidateTokenReq *request,
    ::auth::ValidateTokenResp *response) {

    grpc::ServerUnaryReactor *reactor = context->DefaultReactor();

    const std::string &token = request->token();

    spdlog::info("[AuthService] ValidateToken request: token={}", token);

    if (token.empty()) {
        response->set_valid(false);
        spdlog::warn("[AuthService] ValidateToken failed: empty token");
        reactor->Finish(::grpc::Status::OK);
        return reactor;
    }

    std::string captured_token = token;
    auto &redis = RedisConnector::instance();
    boost::asio::co_spawn(
        redis.get_executor(),
        [reactor, response, captured_token]()
            -> boost::asio::awaitable<void> {
            try {
                std::string key =
                    std::string(TOKEN_KEY_PREFIX) + captured_token;

                auto value_opt =
                    co_await RedisConnector::instance().get(key);

                if (value_opt) {
                    const std::string &value = *value_opt;
                    auto comma_pos = value.find(',');
                    if (comma_pos != std::string::npos) {
                        std::string user_id = value.substr(0, comma_pos);
                        std::string username =
                            value.substr(comma_pos + 1);

                        auto ttl =
                            co_await RedisConnector::instance().ttl(key);

                        response->set_valid(true);
                        response->set_user_id(user_id);
                        response->set_username(username);
                        if (ttl >= 0) {
                            auto now =
                                std::chrono::system_clock::now();
                            auto expires_at =
                                std::chrono::duration_cast<
                                    std::chrono::seconds>(
                                    (now + std::chrono::seconds(ttl))
                                        .time_since_epoch())
                                    .count();
                            response->set_expires_at(expires_at);
                        }

                        spdlog::info(
                            "[AuthService] ValidateToken success: user={}, username={}",
                            user_id, username);
                        reactor->Finish(::grpc::Status::OK);
                        co_return;
                    }
                }

                response->set_valid(false);
                spdlog::warn(
                    "[AuthService] ValidateToken failed: token not found or expired");
                reactor->Finish(::grpc::Status::OK);

            } catch (const std::exception &e) {
                spdlog::error(
                    "[AuthService] ValidateToken Redis error: {}", e.what());
                response->set_valid(false);
                reactor->Finish(::grpc::Status(
                    ::grpc::StatusCode::INTERNAL, "Redis error"));
            }
        },
        boost::asio::detached);

    return reactor;
}