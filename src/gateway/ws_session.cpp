#include "gateway/ws_session.h"
#include "gateway/ws_session_manager.h"
#include "util/redisconnector.h"
#include "util/redishandler.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace asio = boost::asio;
using json = nlohmann::json;

WSSession::WSSession(tcp::socket&& socket)
    : ws_(std::move(socket)) {}

boost::asio::awaitable<void> WSSession::start() {
    // 使用 lambda 启动协程，以便捕获 shared_from_this()
    auto self = shared_from_this();
    asio::co_spawn(
        ws_.get_executor(),
        [self]() -> asio::awaitable<void> {
            boost::system::error_code ec;

            // 1. WebSocket 握手
            co_await self->ws_.async_accept(
                asio::redirect_error(asio::use_awaitable, ec));

            if (ec) {
                spdlog::error("WSSession: WebSocket handshake failed: {}", ec.message());
                co_return;
            }

            spdlog::info("WSSession: WebSocket connection accepted from {}",
                         self->ws_.next_layer().remote_endpoint().address().to_string());

            // 2. 读取认证帧
            co_await self->doReadAuth();
        },
        asio::detached);
}

asio::awaitable<void> WSSession::doReadAuth() {
    auto self = shared_from_this();
    boost::system::error_code ec;

    // 设置合理的超时（认证阶段 10 秒）
    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));

    // 读取第一帧（认证信息）
    co_await ws_.async_read(buffer_,
        asio::redirect_error(asio::use_awaitable, ec));

    if (ec) {
        spdlog::error("WSSession: auth read failed: {}", ec.message());
        co_return;
    }

    // 解析认证 JSON
    std::string authMsg = beast::buffers_to_string(buffer_.data());
    buffer_.consume(buffer_.size());

    try {
        json authJson = json::parse(authMsg);
        if (!authJson.contains("type") || authJson["type"] != "auth" ||
            !authJson.contains("userId") || !authJson.contains("token")) {
            spdlog::warn("WSSession: invalid auth message (missing userId or token): {}", authMsg);

            // 发送错误响应并关闭
            json errResp = {{"type", "error"}, {"message", "missing userId or token"}};
            std::string errStr = errResp.dump();
            co_await ws_.async_write(asio::buffer(errStr),
                asio::redirect_error(asio::use_awaitable, ec));
            co_await ws_.async_close(websocket::close_code::policy_error,
                asio::redirect_error(asio::use_awaitable, ec));
            co_return;
        }

        std::string userId = authJson["userId"].get<std::string>();
        std::string token = authJson["token"].get<std::string>();

        // Token 验证：查询 Redis
        bool token_valid = false;
        try {
            auto& redis = RedisConnector::get_instance().get_redis();
            auto token_value = redis.get("token:" + token);
            if (token_value) {
                // Redis 值格式: user_id,username
                auto comma_pos = token_value->find(',');
                if (comma_pos != std::string::npos) {
                    std::string cached_user_id = token_value->substr(0, comma_pos);
                    if (cached_user_id == userId) {
                        token_valid = true;
                        spdlog::info("WSSession: token verified for user {}", userId);
                    } else {
                        spdlog::warn("WSSession: token user_id mismatch: expected={}, got={}",
                                     userId, cached_user_id);
                    }
                }
            } else {
                spdlog::warn("WSSession: token not found in Redis for user {}", userId);
            }
        } catch (const std::exception& e) {
            spdlog::error("WSSession: Redis token verification error: {}", e.what());
        }

        if (!token_valid) {
            spdlog::warn("WSSession: auth failed for user {} - invalid or expired token", userId);

            json errResp = {{"type", "auth_failed"}, {"message", "invalid or expired token"}};
            std::string errStr = errResp.dump();
            co_await ws_.async_write(asio::buffer(errStr),
                asio::redirect_error(asio::use_awaitable, ec));
            co_await ws_.async_close(websocket::close_code::policy_error,
                asio::redirect_error(asio::use_awaitable, ec));
            co_return;
        }

        userId_ = userId;
        spdlog::info("WSSession: user {} authenticated", userId_);

        // 3. 注册到 SessionManager
        WSSessionManager::instance().registerSession(userId_, self);

        // 4. 更新 Redis 在线状态
        try {
            auto& redis = RedisConnector::get_instance().get_redis();
            redis.setex("user:" + userId_ + ":online", 300, "1"); // 5分钟过期（靠心跳续期）
        } catch (const std::exception& e) {
            spdlog::error("WSSession: failed to update Redis online status: {}", e.what());
        }

        // 5. 发送认证成功响应
        json okResp = {{"type", "auth_ok"}, {"userId", userId_}};
        std::string okStr = okResp.dump();
        co_await ws_.async_write(asio::buffer(okStr),
            asio::redirect_error(asio::use_awaitable, ec));

        if (ec) {
            spdlog::error("WSSession: failed to send auth_ok: {}", ec.message());
            onDisconnect();
            co_return;
        }

        // 6. 进入消息循环
        co_await doReadLoop();

    } catch (const json::parse_error& e) {
        spdlog::error("WSSession: JSON parse error: {}", e.what());
    }
}

asio::awaitable<void> WSSession::doReadLoop() {
    auto self = shared_from_this();
    boost::system::error_code ec;

    // 设置更长的超时（心跳阶段）
    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));

    for (;;) {
        // 读取客户端消息（心跳 / ping / 业务消息）
        co_await ws_.async_read(buffer_,
            asio::redirect_error(asio::use_awaitable, ec));

        if (ec) {
            if (ec == websocket::error::closed ||
                ec == asio::error::eof ||
                ec == asio::error::connection_reset) {
                spdlog::info("WSSession: user {} connection closed", userId_);
            } else {
                spdlog::error("WSSession: read error for user {}: {}", userId_, ec.message());
            }
            break;
        }

        std::string msg = beast::buffers_to_string(buffer_.data());
        buffer_.consume(buffer_.size());

        // 处理心跳 / ping
        try {
            json msgJson = json::parse(msg);
            if (msgJson.contains("type")) {
                std::string type = msgJson["type"];
                if (type == "ping") {
                    json pongResp = {{"type", "pong"}};
                    std::string pongStr = pongResp.dump();
                    co_await ws_.async_write(asio::buffer(pongStr),
                        asio::redirect_error(asio::use_awaitable, ec));
                    if (ec) {
                        spdlog::error("WSSession: pong write error: {}", ec.message());
                        break;
                    }

                    // 心跳续期 Redis 在线状态
                    try {
                        auto& redis = RedisConnector::get_instance().get_redis();
                        redis.setex("user:" + userId_ + ":online", 300, "1");
                    } catch (const std::exception& e) {
                        spdlog::error("WSSession: Redis heartbeat update error: {}", e.what());
                    }
                    continue;
                }
            }
        } catch (const json::parse_error&) {
            // 非 JSON 消息，忽略
        }

        spdlog::debug("WSSession: received message from user {}: {}", userId_, msg);
    }

    onDisconnect();
}

void WSSession::asyncSend(std::shared_ptr<std::string> payload) {
    // 通过 io_context::post 投递到 WebSocket 所在的 strand，保证线程安全
    auto self = shared_from_this();
    asio::post(ws_.get_executor(),
        [self, payload]() {
            self->ws_.async_write(
                asio::buffer(*payload),
                [self, payload](boost::system::error_code ec, std::size_t /*bytes*/) {
                    if (ec) {
                        spdlog::error("WSSession: asyncSend error for user {}: {}",
                                      self->userId_, ec.message());
                    } else {
                        spdlog::debug("WSSession: asyncSend success for user {}, size={}",
                                      self->userId_, payload->size());
                    }
                });
        });
}

void WSSession::onDisconnect() {
    if (userId_.empty()) return;

    spdlog::info("WSSession: user {} disconnecting, cleaning up", userId_);

    // 1. 从 SessionManager 注销
    WSSessionManager::instance().unregisterSession(userId_);

    // 2. 更新 Redis 为离线
    try {
        auto& redis = RedisConnector::get_instance().get_redis();
        redis.set("user:" + userId_ + ":online", "0");
    } catch (const std::exception& e) {
        spdlog::error("WSSession: Redis offline update error: {}", e.what());
    }

    // 3. 尝试优雅关闭 WebSocket
    ws_.async_close(websocket::close_code::normal,
        [](boost::system::error_code ec) {
            if (ec) {
                spdlog::debug("WSSession: graceful close error: {}", ec.message());
            }
        });
}