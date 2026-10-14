#include "services/auth_service/server.h"
#include "user/userinfo.h"
#include "storage/mysqlconnector.h"
#include "storage/mysqlhandler.h"
#include "storage/redisconnector.h"
#include "util/account_pool.h"
#include <algorithm>
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

::grpc::ServerUnaryReactor* AuthServiceImpl::Register(
    ::grpc::CallbackServerContext* context,
    const ::auth::RegisterRequest* request,
    ::auth::AuthRegResponse* response)
{
    auto* reactor = context->DefaultReactor();

    const auto id = context->client_metadata().find("x-registration-id");
    const std::string requestId = id == context->client_metadata().end() ? "" :
        std::string(id->second.data(), id->second.size());
    const bool validId = !requestId.empty() && requestId.size() <= 64 &&
        std::all_of(requestId.begin(), requestId.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-';
        });

    const auto phone = request->phone();
    const bool validPhone = phone.size() == 11 && phone[0] == '1' &&
        phone[1] >= '3' && phone[1] <= '9' &&
        std::all_of(phone.begin(), phone.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });

    if (!validId || !validPhone || request->nickname().empty() || request->nickname().size() > 256 ||
        request->password().empty() || request->password().size() > 1024) {
        response->set_success(false);
        response->set_errorcode("INVALID_INFO");
        response->set_errormessage("Valid registration request ID, phone, nickname and password are required");
        reactor->Finish(::grpc::Status::OK);
        return reactor;
    }

    auto task = [](std::string id, std::string phone, std::string nickname, std::string password,
                   ::auth::AuthRegResponse* response) -> boost::asio::awaitable<void> {
        MySQLHandler::RegistrationInput input;
        input.request_id = std::move(id);
        input.payload = MySQLHandler::registration_payload(phone, nickname, password);
        input.user.phone = std::move(phone);
        input.user.nickname = std::move(nickname);
        input.user.password_salt = generate_salt();
        input.user.password_hash = hash_password(password, input.user.password_salt);
        const auto result = co_await MySQLHandler::register_user(MySQLConnector::instance().get_pool(0), input);
        response->set_success(true);
        auto* user = response->mutable_user();
        user->set_userid(result.user_id);
        user->set_account(result.account);
        user->set_phone(input.user.phone);
        user->set_nickname(input.user.nickname);
    };
    try {
        // Pool 0 is not thread-safe; all operations must run on its executor.
        boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(0),
            task(requestId, phone, request->nickname(), request->password(), response),
            [reactor, response](std::exception_ptr error) {
                if (error) {
                    response->Clear();
                    response->set_success(false);
                    try { 
                        std::rethrow_exception(error); 
                    } catch (const MySQLHandler::RegistrationError& e) {        // 已定义的业务异常
                        response->set_errorcode(e.code);
                        response->set_errormessage(e.what());
                    } catch (const std::exception& e) {               // 任何继承std::exception但不是MySQLHandler::RegistrationError的异常
                        spdlog::warn(e.what());
                        response->set_errorcode("DB_ERROR");
                        response->set_errormessage("Registration could not be completed; retry the same request ID");
                    } catch (...) {                                 // 未知异常
                        response->set_errorcode("INNER_ERROR");
                        response->set_errormessage("Registration could not be completed");
                    }
                }
                reactor->Finish(::grpc::Status::OK);
            });
    } catch (...) {
        response->set_success(false);
        response->set_errorcode("INNER_ERROR");
        response->set_errormessage("Unable to start registration");
        reactor->Finish(::grpc::Status::OK);
    }
    return reactor;
}
::grpc::ServerUnaryReactor *AuthServiceImpl::Login(
    ::grpc::CallbackServerContext *context,
    const ::auth::LoginRequest *request,
    ::auth::AuthRegResponse *response) {

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
                    response->set_errormessage("Invalid username");
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
    ::auth::ValidateTokenResponse *response) {

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
