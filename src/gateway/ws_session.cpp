#include "gateway/ws_session.h"
#include "gateway/ws_session_manager.h"
#include "services/auth_service/client.h"
#include "services/msg_service/client.h"
#include "services/push_service/client.h"
#include "msg.pb.h"
#include "push.pb.h"
#include "sdkws.pb.h"
#include "util/otel_tracer.h"
#include <opentelemetry/trace/scope.h>
#include "util/redisconnector.h"
#include "util/redishandler.h"
#include "util/mongohandler.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <spdlog/spdlog.h>

namespace asio = boost::asio;

WSSession::WSSession(tcp::socket&& socket, AuthClient* auth_client,
                     MsgClient* msg_client, PushClient* push_client)
    : ws_(std::move(socket)), auth_client_(auth_client), msg_client_(msg_client),
      push_client_(push_client) {}

WSSession::~WSSession() = default;

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
    co_return;
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

    // 解析认证 Protobuf 二进制帧
    std::string authData = beast::buffers_to_string(buffer_.data());
    buffer_.consume(buffer_.size());

    // 尝试解析 sdkws::SdkWSReq
    sdkws::SdkWSReq authReq;
    if (!authReq.ParseFromString(authData) || authReq.token().empty() || authReq.userid().empty()) {
        spdlog::warn("WSSession: invalid auth protobuf message (missing token or userID), raw_size={}",
                     authData.size());

        // 发送错误响应（Protobuf 二进制）
        sdkws::SdkWSResp errResp;
        errResp.set_errorcode("1");
        errResp.set_errormsg("missing token or userID in SdkWSReq");
        errResp.set_type(0);  // error type
        std::string errBin = errResp.SerializeAsString();

        ws_.binary(true);
        co_await ws_.async_write(asio::buffer(errBin),
            asio::redirect_error(asio::use_awaitable, ec));
        co_await ws_.async_close(websocket::close_code::policy_error,
            asio::redirect_error(asio::use_awaitable, ec));
        co_return;
    }

    std::string userId = authReq.userid();
    std::string token = authReq.token();

    spdlog::info("WSSession: auth request from user={}, reqType={}, trackID={}",
                 userId, authReq.type(), authReq.trackid());

    // Token 验证：通过 gRPC 调用 AuthService
    bool token_valid = false;
    std::string username;
    try {
        auto resp = auth_client_->ValidateToken(token);
        if (resp.valid() && resp.user_id() == userId) {
            token_valid = true;
            username = resp.username();
            spdlog::info("WSSession: token verified via AuthService for user {} (username: {})",
                         userId, username);
        } else {
            spdlog::warn("WSSession: AuthService token validation failed: valid={}, resp_user_id={}, request_user_id={}",
                         resp.valid(), resp.user_id(), userId);
        }
    } catch (const std::exception& e) {
        spdlog::error("WSSession: AuthService ValidateToken error: {}", e.what());
    }

    if (!token_valid) {
        spdlog::warn("WSSession: auth failed for user {} - invalid or expired token", userId);

        sdkws::SdkWSResp errResp;
        errResp.set_errorcode("401");
        errResp.set_errormsg("invalid or expired token");
        errResp.set_userid(userId);
        errResp.set_type(0);  // error type
        std::string errBin = errResp.SerializeAsString();

        ws_.binary(true);
        co_await ws_.async_write(asio::buffer(errBin),
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

    // 5. 发送认证成功响应（Protobuf 二进制）
    sdkws::SdkWSResp okResp;
    okResp.set_errorcode("0");
    okResp.set_errormsg("auth success");
    okResp.set_userid(userId_);
    okResp.set_type(0);  // auth_ok
    // 回传 requestId 和 token 以便客户端匹配
    okResp.set_requestid(authReq.requestid());
    okResp.set_token(token);
    okResp.set_deviceid(authReq.deviceid());
    std::string okBin = okResp.SerializeAsString();

    ws_.binary(true);
    co_await ws_.async_write(asio::buffer(okBin),
        asio::redirect_error(asio::use_awaitable, ec));

    if (ec) {
        spdlog::error("WSSession: failed to send auth_ok: {}", ec.message());
        onDisconnect();
        co_return;
    }

    // 6. 启动消息读循环（与离线推送并发运行，处理客户端的 ACK）
    asio::co_spawn(
        ws_.get_executor(),
        [self]() -> asio::awaitable<void> {
            co_await self->doReadLoop();
        },
        asio::detached);

    // 7. 分段拉取并推送离线消息（每批等待 ACK 后再拉下一批）
    try {
        co_await pullAndPushOfflineMsgs();
    } catch (const std::exception& e) {
        spdlog::error("WSSession: pullAndPushOfflineMsgs error for user {}: {}", userId_, e.what());
    }
    // doReadLoop 已在后台运行，无需再次启动
}

asio::awaitable<void> WSSession::doReadLoop() {
    auto self = shared_from_this();
    boost::system::error_code ec;

    // 设置更长的超时（心跳阶段）
    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));

    for (;;) {
        // 读取客户端消息（心跳 / 业务消息，二进制 Protobuf 帧）
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

        std::string msgData = beast::buffers_to_string(buffer_.data());
        buffer_.consume(buffer_.size());

        // 尝试解析 SdkWSReq（二进制 Protobuf）
        sdkws::SdkWSReq req;
        if (!req.ParseFromString(msgData)) {
            spdlog::debug("WSSession: received non-protobuf message from user {}, size={}",
                          userId_, msgData.size());
            continue;
        }

        int32_t reqType = req.type();
        spdlog::debug("WSSession: received req from user {}: type={}, requestId={}",
                      userId_, reqType, req.requestid());

        // 处理心跳 ping（type=0 约定为 ping）
        if (reqType == 0) {
            sdkws::SdkWSResp pongResp;
            pongResp.set_requestid(req.requestid());
            pongResp.set_errorcode("0");
            pongResp.set_errormsg("pong");
            pongResp.set_userid(userId_);
            pongResp.set_type(0);
            std::string pongBin = pongResp.SerializeAsString();

            ws_.binary(true);
            co_await ws_.async_write(asio::buffer(pongBin),
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

        // type=101: 发送消息
        if (reqType == 101) {
            sdkws::SendMessageReq sendMsgReq;
            std::string respBin;
            bool hasResp = false;

            if (sendMsgReq.ParseFromString(req.data())) {

                // ── OTel: 创建 Root Span,作为整条 Trace 的起点 ──
                auto tracer = ape::otel::GetTracer();
                opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> sendSpan;
                std::unique_ptr<opentelemetry::trace::Scope> sendScope;
                if (tracer) {
                    sendSpan = tracer->StartSpan(
                        "WS /msg/send",
                        {{"ws.message_type", "send_msg"},
                         {"ws.user_id", userId_},
                         {"ws.msg_size_count", sendMsgReq.msgs_size()}});
                    //将sendScope attach到OTel线程上下文，供其余函数GetSpan
                    sendScope = std::make_unique<opentelemetry::trace::Scope>(sendSpan);
                }

                try {
                    auto sendResp = msg_client_->SendMessages(sendMsgReq);

                    sdkws::SdkWSResp wsResp;
                    wsResp.set_requestid(req.requestid());
                    wsResp.set_errorcode("0");
                    wsResp.set_errormsg("send message success");
                    wsResp.set_userid(userId_);
                    wsResp.set_type(101);
                    wsResp.set_data(sendResp.SerializeAsString());
                    respBin = wsResp.SerializeAsString();
                    hasResp = true;
                } catch (const std::exception& e) {
                    spdlog::error("WSSession: SendMessages failed for user {}: {}", userId_, e.what());

                    sdkws::SdkWSResp errResp;
                    errResp.set_requestid(req.requestid());
                    errResp.set_errorcode("500");
                    errResp.set_errormsg(std::string("SendMessages failed: ") + e.what());
                    errResp.set_userid(userId_);
                    errResp.set_type(101);
                    respBin = errResp.SerializeAsString();
                    hasResp = true;
                }

                // End the span
                if (sendSpan) {
                    sendScope.reset();
                    sendSpan->End();
                }
            } else {
                spdlog::warn("WSSession: failed to parse SendMessageReq from user {}", userId_);
            }

            // 统一写回响应（将 co_await 移出 catch 块以避免协程限制）
            if (hasResp) {
                ws_.binary(true);
                co_await ws_.async_write(asio::buffer(respBin),
                    asio::redirect_error(asio::use_awaitable, ec));
                if (ec) {
                    spdlog::error("WSSession: type=101 response write error: {}", ec.message());
                    break;
                }
            }
            continue;
        }

        // type=106: 统一消息 ACK（在线/离线消息确认，由客户端统一使用）
        if (reqType == 106) {
            handleAck(req);
            continue;
        }

        // 其他业务消息类型暂不做处理，仅记录日志
        spdlog::debug("WSSession: received business message from user {}: type={}, requestId={}",
                      userId_, reqType, req.requestid());
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

asio::awaitable<void> WSSession::pullAndPushOfflineMsgs() {
    auto self = shared_from_this();
    boost::system::error_code ec;
    constexpr int kBatchSize = 50;
    spdlog::info("WSSession: pulling offline messages for user {} (batch_size={})", userId_, kBatchSize);

    // 1. 获取用户上次接收的最大 seq
    int64_t lastSeq = 0;
    try {
        auto& redis = RedisConnector::get_instance().get_redis();
        auto val = redis.get("user:" + userId_ + ":last_seq");
        if (val && !val->empty()) {
            lastSeq = std::stoll(*val);
        }
    } catch (const std::exception& e) {
        spdlog::warn("WSSession: failed to get last_seq for user {}: {}", userId_, e.what());
    }

    // 2. 循环分段拉取：每批最多 kBatchSize 条，等客户端 ACK 后再拉下一批
    for (;;) {
        // 从 Redis 热存储拉取当前批次
        auto batchMsgs = RedisHandler::GetOfflineMsgs(userId_, lastSeq, kBatchSize);

        // Redis 不足时从 MongoDB 冷存储补充
        if (static_cast<int>(batchMsgs.size()) < kBatchSize) {
            int remaining = kBatchSize - static_cast<int>(batchMsgs.size());
            auto coldMsgs = MongoHandler::GetOfflineMsgsFromMongo(userId_, lastSeq, remaining);
            for (auto& msg : coldMsgs) {
                batchMsgs.push_back(std::move(msg));
            }
        }

        // 没有更多消息，结束拉取
        if (batchMsgs.empty()) {
            spdlog::info("WSSession: no more offline messages for user {}", userId_);
            break;
        }

        spdlog::info("WSSession: batch pulling {} offline messages for user {}, lastSeq={}",
                     batchMsgs.size(), userId_, lastSeq);

        // 3. 逐条推送给客户端，记录待 ACK 映射
        int64_t batchMaxSeq = lastSeq;
        for (const auto& msg : batchMsgs) {
            sdkws::SdkWSResp pushResp;
            pushResp.set_errorcode("0");
            pushResp.set_errormsg("offline message");
            pushResp.set_userid(userId_);
            pushResp.set_type(200);  // type 200 = 离线消息推送
            pushResp.set_requestid(msg.servermsgid());
            pushResp.set_data(msg.SerializeAsString());
            std::string pushBin = pushResp.SerializeAsString();

            ws_.binary(true);
            co_await ws_.async_write(asio::buffer(pushBin),
                asio::redirect_error(asio::use_awaitable, ec));

            if (ec) {
                spdlog::error("WSSession: failed to push offline msg to user {}: {}, msgId={}",
                              userId_, ec.message(), msg.servermsgid());
                co_return;  // 连接已断，停止拉取
            }

            pendingOfflineMsgs_[msg.servermsgid()] = msg.seq();
            if (msg.seq() > batchMaxSeq) {
                batchMaxSeq = msg.seq();
            }
            spdlog::debug("WSSession: pushed offline msg to user {}, msgId={}, seq={}",
                          userId_, msg.servermsgid(), msg.seq());
        }

        // 4. 轮询等待客户端 ACK 清空本批次（doReadLoop 会并发处理 ACK）
        //    使用 steady_timer 每 200ms 检查一次，避免忙等
        spdlog::info("WSSession: waiting for client ACK on {} msgs for user {}",
                     pendingOfflineMsgs_.size(), userId_);

        asio::steady_timer timer(ws_.get_executor());
        timer.expires_after(std::chrono::milliseconds(200));

        while (!pendingOfflineMsgs_.empty()) {
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
            // 连接断开时 doReadLoop 会清理 pendingOfflineMsgs_，直接退出
            if (ec) {
                spdlog::error("WSSession: timer error while waiting ACK: {}", ec.message());
                co_return;
            }
            timer.expires_after(std::chrono::milliseconds(200));
        }

        spdlog::info("WSSession: batch ACK done for user {}, batchMaxSeq={}", userId_, batchMaxSeq);

        // 5. 推进 lastSeq 到本批次最大值，继续下一批
        lastSeq = batchMaxSeq;
    }

    spdlog::info("WSSession: offline push complete for user {}", userId_);
}

void WSSession::handleAck(const sdkws::SdkWSReq& req) {
    // 解析 ACK 请求
    sdkws::AckReq ackReq;
    if (!ackReq.ParseFromString(req.data())) {
        spdlog::warn("WSSession: failed to parse AckReq from user {}", userId_);
        return;
    }

    spdlog::info("WSSession: unified ACK from user {}, {} msgIds, ackType={}",
                 userId_, ackReq.servermsgids_size(), ackReq.acktype());

    // 遍历所有已确认的 serverMsgIDs
    for (int i = 0; i < ackReq.servermsgids_size(); ++i) {
        std::string serverMsgID = ackReq.servermsgids(i);

        // 先检查是否是离线消息 ACK（在 pendingOfflineMsgs_ 中）
        auto it = pendingOfflineMsgs_.find(serverMsgID);
        if (it != pendingOfflineMsgs_.end()) {
            int64_t seq = it->second;

            // Redis ACK 清理
            RedisHandler::AckOfflineMsg(userId_, serverMsgID);

            // 更新 last_seq
            try {
                auto& redis = RedisConnector::get_instance().get_redis();
                auto oldVal = redis.get("user:" + userId_ + ":last_seq");
                int64_t oldSeq = (oldVal && !oldVal->empty()) ? std::stoll(*oldVal) : 0;
                if (seq > oldSeq) {
                    redis.set("user:" + userId_ + ":last_seq", std::to_string(seq));
                }
            } catch (const std::exception& e) {
                spdlog::error("WSSession: failed to update last_seq on ACK for user {}: {}", userId_, e.what());
            }

            pendingOfflineMsgs_.erase(it);
            spdlog::debug("WSSession: offline msg ACKed via type 106, msgId={}, seq={}, remaining={}",
                          serverMsgID, seq, pendingOfflineMsgs_.size());
        }

        // 通过 gRPC 转发 ACK 给 PushService（处理在线/离线消息的最终确认）
        try {
            ::push::AckMsgReq ackMsgReq;
            ackMsgReq.set_userid(userId_);
            ackMsgReq.set_servermsgid(serverMsgID);
            ackMsgReq.set_seq(0);          // seq 未知时填0，PushService 自行查 Redis
            ackMsgReq.set_acktype(ackReq.acktype());
            push_client_->AckMsg(ackMsgReq);
        } catch (const std::exception& e) {
            spdlog::error("WSSession: AckMsg gRPC failed for user {}, msgId={}: {}",
                         userId_, serverMsgID, e.what());
        }
    }
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
