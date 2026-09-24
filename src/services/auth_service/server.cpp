#include "services/auth_service/server.h"
#include "user/userinfo.h"
#include "storage/mysqlconnector.h"
#include "storage/mysqlhandler.h"
#include "storage/redisconnector.h"
#include "util/account_pool.h"
#include "util/uuid.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <cstdint>
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
// Register — 全异步：取号 + 写入 MySQL + Redis token
// ──────────────────────────────────────────
::grpc::ServerUnaryReactor *AuthServiceImpl::Register(
    ::grpc::CallbackServerContext *context,
    const ::auth::RegisterRequest *request,
    ::auth::AuthResponse *response) {

    grpc::ServerUnaryReactor *reactor = context->DefaultReactor();

    const std::string password = request->password();
    const std::string nickname = request->nickname();
    const std::string phone    = request->phone();

    spdlog::info("[AuthService] Register request: nickname={}", nickname);

    // 参数校验（同步检查，无 IO）
    if (nickname.empty() || password.empty()) {
        response->set_success(false);
        response->set_errorcode("INVALID_PARAM");
        response->set_errormessage("Nickname and password are required");
        spdlog::warn("[AuthService] Register failed: empty nickname or password");
        reactor->Finish(::grpc::Status::OK);
        return reactor;
    }

    // 准备用户数据（account 由取号获得，user_id 由数据库自增返回）
    userInfo user;
    user.user_id = 0;      // 插入后回填
    user.account = 0;      // 取号后赋值
    user.nickname = nickname;
    user.phone = phone;
    user.password_salt = generate_salt();
    user.password_hash = hash_password(password, user.password_salt);

    std::string token = generate_token();
    int64_t expires_at = calculate_expiry();

    boost::asio::co_spawn(
        MySQLConnector::instance().get_executor(),
        [reactor, response, user = std::move(user), token, expires_at] () mutable
            -> boost::asio::awaitable<void> {
            bool failed = false;

            try {
                // ── 1. 插入用户，拿到自增 user_id ──
                auto inserted_id = co_await MySQLHandler::insert_user(user);
                if (!inserted_id.has_value()) {
                    response->set_success(false);
                    response->set_errorcode("DB_ERROR");
                    response->set_errormessage("Failed to create user in database");
                    spdlog::error("[AuthService] Register failed: DB insert error");
                    reactor->Finish(::grpc::Status::OK);
                    co_return;
                }
                user.user_id = inserted_id.value();

                // ── 2. 从账号池取号 ──
                uint64_t account = co_await AccountPoolTool::acquire();
                if (account == 0) {
                    // 池空，紧急补货后再取一次
                    spdlog::warn("[AuthService] Account pool empty, emergency refill");
                    co_await AccountPoolTool::refill(
                        AccountPoolAllocator::kRefillBatch);
                    account = co_await AccountPoolTool::acquire();
                    if (account == 0) {
                        response->set_success(false);
                        response->set_errorcode("ACCOUNT_POOL_EMPTY");
                        response->set_errormessage(
                            "Account pool exhausted, please retry later");
                        spdlog::error(
                            "[AuthService] Register failed: account pool empty, user_id={}",
                            user.user_id);
                        reactor->Finish(::grpc::Status::OK);
                        co_return;
                    }
                }
                user.account = account;

                // ── 3. 把 account 写回 user 表 ──
                bool updated = co_await MySQLHandler::update_account(
                    user.user_id, user.account);
                if (!updated) {
                    response->set_success(false);
                    response->set_errorcode("DB_ERROR");
                    response->set_errormessage("Failed to persist account number");
                    spdlog::error(
                        "[AuthService] Register failed: update account error, user_id={}",
                        user.user_id);
                    reactor->Finish(::grpc::Status::OK);
                    co_return;
                }

                // ── 4. 持久化 token 到 Redis ──
                std::string key = std::string(TOKEN_KEY_PREFIX) + token;
                std::string value =
                    std::to_string(user.user_id) + "," +
                    std::to_string(user.account);
                co_await RedisConnector::instance().setex(
                    key, TOKEN_TTL_SECONDS, value);
                spdlog::info(
                    "[AuthService] Token persisted: user_id={}, account={}",
                    user.user_id, user.account);

                // ── 5. 构造响应 ──
                response->set_success(true);
                response->set_token(token);
                response->set_expiresat(expires_at);
                auto *auth_user = response->mutable_user();
                auth_user->set_userid(user.user_id);
                auth_user->set_account(user.account);
                auth_user->set_nickname(user.nickname);

                spdlog::info(
                    "[AuthService] User registered: user_id={}, account={}",
                    user.user_id, user.account);
                reactor->Finish(::grpc::Status::OK);

            } catch (const std::exception &e) {
                failed = true;
                spdlog::error(
                    "[AuthService] Register coroutine exception: {}", e.what());
            }

            // catch 里不能 co_await，失败处理放在这里
            if (failed) {
                response->set_success(false);
                response->set_errorcode("INTERNAL_ERROR");
                response->set_errormessage("Internal error");
                reactor->Finish(::grpc::Status(
                    ::grpc::StatusCode::INTERNAL, "internal error"));
                co_return;
            }

            // ── 6. 触发懒补货（异步，不阻塞本次注册）──
            boost::asio::co_spawn(
                MySQLConnector::instance().get_executor(),
                AccountPoolTool::ensure_pool_not_low(),
                boost::asio::detached);
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

    const uint64_t account = request->account();    // 0即是字符串的空
    const std::string password = request->password();

    spdlog::info("[AuthService] Login request: account={}", account);

    if (account == 0 || password.empty()) {
        response->set_success(false);
        response->set_errorcode("INVALID_PARAM");
        response->set_errormessage("account and password are required");
        spdlog::warn("[AuthService] Login failed: empty account or password");
        reactor->Finish(::grpc::Status::OK);
        return reactor;
    }

    std::string token = generate_token();
    int64_t expires_at = calculate_expiry();

    boost::asio::co_spawn(
        MySQLConnector::instance().get_executor(),
        [reactor, response, account, password, token, expires_at, this]()
            -> boost::asio::awaitable<void> {
            try {
                // ── 查询用户 ──
                auto user_opt = co_await MySQLHandler::find_user_by_account(account);
                if (!user_opt.has_value()) {
                    response->set_success(false);
                    response->set_errorcode("USER_NOT_FOUND");
                    response->set_errormessage("Invalid username or password");
                    spdlog::warn("[AuthService] Login failed: account {} not found",
                                 account);
                    reactor->Finish(::grpc::Status::OK);
                    co_return;
                }

                const auto &user = user_opt.value();

                // ── 验证密码 ──
                std::string input_hash = hash_password(password, user.password_salt);
                if (input_hash != user.password_hash) {
                    response->set_success(false);
                    response->set_errorcode("WRONG_PASSWORD");
                    response->set_errormessage("Invalid account or password");
                    spdlog::warn(
                        "[AuthService] Login failed: wrong password for account={}",
                        account);
                    reactor->Finish(::grpc::Status::OK);
                    co_return;
                }

                // ── 更新最后登录时间 ──
                co_await MySQLHandler::update_last_login(user.user_id);

                // ── 持久化 token 到 Redis ──
                std::string key = std::string(TOKEN_KEY_PREFIX) + token;
                std::string value = std::to_string(user.user_id) + "," + std::to_string(user.account);
                co_await RedisConnector::instance().setex(key, TOKEN_TTL_SECONDS, value);
                spdlog::info(
                    "[AuthService] Token persisted to Redis: user={}, token={}",
                    user.user_id, token);

                // ── 构造响应 ──
                response->set_success(true);
                response->set_token(token);
                response->set_expiresat(expires_at);
                auto *auth_user = response->mutable_user();
                auth_user->set_userid(user.user_id);
                auth_user->set_account(user.account);
                auth_user->set_nickname(user.nickname);

                spdlog::info("[AuthService] User logged in: {} ({})",
                             user.account, user.user_id);
                reactor->Finish(::grpc::Status::OK);

            } catch (const std::exception &e) {
                spdlog::error("[AuthService] Login coroutine exception: {}", e.what());
                response->set_success(false);
                response->set_errorcode("INTERNAL_ERROR");
                response->set_errormessage(e.what());
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

                if (!value_opt) {
                    response->set_valid(false);
                    spdlog::warn(
                        "[AuthService] ValidateToken failed: token not found or expired");
                    reactor->Finish(::grpc::Status::OK);
                    co_return;
                }

                const std::string &value = *value_opt;
                auto comma_pos = value.find(',');
                if (comma_pos == std::string::npos) {
                    response->set_valid(false);
                    spdlog::error(
                        "[AuthService] ValidateToken failed: malformed value={}",
                        value);
                    reactor->Finish(::grpc::Status::OK);
                    co_return;
                }

                // 解析 user_id,account
                uint64_t user_id = std::stoull(value.substr(0, comma_pos));
                uint64_t account = std::stoull(value.substr(comma_pos + 1));

                auto ttl = co_await RedisConnector::instance().ttl(key);

                response->set_valid(true);
                response->set_userid(user_id);
                response->set_account(account);
                if (ttl >= 0) {
                    auto now = std::chrono::system_clock::now();
                    auto expires_at = std::chrono::duration_cast<
                        std::chrono::seconds>(
                        (now + std::chrono::seconds(ttl)).time_since_epoch())
                        .count();
                    response->set_expiresat(expires_at);
                }

                spdlog::info(
                    "[AuthService] ValidateToken success: user_id={}, account={}",
                    user_id, account);
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