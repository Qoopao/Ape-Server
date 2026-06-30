#include "gateway/ws_session.h"
#include "gateway/ws_session_manager.h"
#include "msg.pb.h"
#include "push.pb.h"
#include "sdkws.pb.h"
#include "services/auth_service/client.h"
#include "services/msg_service/client.h"
#include "services/push_service/client.h"
#include "util/mongohandler.h"
#include "util/otel_tracer.h"
#include "util/redisconnector.h"
#include "util/redishandler.h"
#include <opentelemetry/trace/scope.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <spdlog/spdlog.h>

namespace asio = boost::asio;

WSSession::WSSession(tcp::socket &&socket, AuthClient *auth_client,
                     MsgClient *msg_client, PushClient *push_client)
    : ws_(std::move(socket)), auth_client_(auth_client),
      msg_client_(msg_client), push_client_(push_client) {}

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
          spdlog::error("WSSession: WebSocket handshake failed: {}",
                        ec.message());
          co_return;
        }

        spdlog::info(
            "WSSession: WebSocket connection accepted from {}",
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
  ws_.set_option(
      websocket::stream_base::timeout::suggested(beast::role_type::server));

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
  if (!authReq.ParseFromString(authData) || authReq.token().empty() ||
      authReq.userid().empty()) {
    spdlog::warn("WSSession: invalid auth protobuf message (missing token or "
                 "userID), raw_size={}",
                 authData.size());

    // 发送错误响应（Protobuf 二进制）
    sdkws::SdkWSResp errResp;
    errResp.set_errorcode("1");
    errResp.set_errormsg("missing token or userID in SdkWSReq");
    errResp.set_type(0); // error type
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
    auto resp = co_await auth_client_->ValidateToken(token);
    if (resp.valid() && resp.user_id() == userId) {
      token_valid = true;
      username = resp.username();
      spdlog::info("WSSession: token verified via AuthService for user {} "
                   "(username: {})",
                   userId, username);
    } else {
      spdlog::warn("WSSession: AuthService token validation failed: valid={}, "
                   "resp_user_id={}, request_user_id={}",
                   resp.valid(), resp.user_id(), userId);
    }
  } catch (const std::exception &e) {
    spdlog::error("WSSession: AuthService ValidateToken error: {}", e.what());
  }

  if (!token_valid) {
    spdlog::warn(
        "WSSession: auth failed for user {} - invalid or expired token",
        userId);

    sdkws::SdkWSResp errResp;
    errResp.set_errorcode("401");
    errResp.set_errormsg("invalid or expired token");
    errResp.set_userid(userId);
    errResp.set_type(0); // error type
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

  // 启动写队列（此后所有 ws_.async_write 统一走 queuedWrite，避免并发写）
  startWriteQueue();

  // 4. 更新 Redis 在线状态
  try {
    co_await RedisConnector::instance().setex(
        "user:" + userId_ + ":online", 300, "1"); // 5分钟过期（靠心跳续期）
  } catch (const std::exception &e) {
    spdlog::error("WSSession: failed to update Redis online status: {}",
                  e.what());
  }

  // 5. 发送认证成功响应（Protobuf 二进制）
  sdkws::SdkWSResp okResp;
  okResp.set_errorcode("0");
  okResp.set_errormsg("auth success");
  okResp.set_userid(userId_);
  okResp.set_type(0); // auth_ok
  // 回传 requestId 和 token 以便客户端匹配
  okResp.set_requestid(authReq.requestid());
  okResp.set_token(token);
  okResp.set_deviceid(authReq.deviceid());
  std::string okBin = okResp.SerializeAsString();

  ws_.binary(true);
  co_await queuedWrite(std::make_shared<std::string>(std::move(okBin)));

  // auth_ok 写失败通过 doDrainWriteQueue 内部处理，此处不再单独检测
  // 后续 doReadLoop / pullAndPushOfflineMsgs 中的写失败同理

  // 6. 启动消息读循环（与离线推送并发运行，处理客户端的 ACK）
  asio::co_spawn(
      ws_.get_executor(),
      [self]() -> asio::awaitable<void> { co_await self->doReadLoop(); },
      asio::detached);

  // 7. 分段拉取并推送离线消息（每批等待 ACK 后再拉下一批）
  try {
    co_await pullAndPushOfflineMsgs();
  } catch (const std::exception &e) {
    spdlog::error("WSSession: pullAndPushOfflineMsgs error for user {}: {}",
                  userId_, e.what());
  }
  // doReadLoop 已在后台运行，无需再次启动
}

asio::awaitable<void> WSSession::doReadLoop() {
  auto self = shared_from_this();
  boost::system::error_code ec;

  // 设置更长的超时（心跳阶段）
  ws_.set_option(
      websocket::stream_base::timeout::suggested(beast::role_type::server));

  for (;;) {
    // 读取客户端消息（心跳 / 业务消息，二进制 Protobuf 帧）
    co_await ws_.async_read(buffer_,
                            asio::redirect_error(asio::use_awaitable, ec));

    if (ec) {
      if (ec == websocket::error::closed || ec == asio::error::eof ||
          ec == asio::error::connection_reset) {
        spdlog::info("WSSession: user {} connection closed", userId_);
      } else {
        spdlog::error("WSSession: read error for user {}: {}", userId_,
                      ec.message());
      }
      break;
    }

    std::string msgData = beast::buffers_to_string(buffer_.data());
    buffer_.consume(buffer_.size());

    // 尝试解析 SdkWSReq（二进制 Protobuf）
    sdkws::SdkWSReq req;
    if (!req.ParseFromString(msgData)) {
      spdlog::debug(
          "WSSession: received non-protobuf message from user {}, size={}",
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
      co_await queuedWrite(
          std::make_shared<std::string>(std::move(pongBin)));

      // 心跳续期 Redis 在线状态
      try {
        co_await RedisConnector::instance().setex(
            "user:" + userId_ + ":online", 300, "1"); // 5分钟过期（靠心跳续期）
      } catch (const std::exception &e) {
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
              "WS /msg/send", {{"ws.message_type", "send_msg"},
                               {"ws.user_id", userId_},
                               {"ws.msg_size_count", sendMsgReq.msgs_size()}});
          // 将sendScope attach到OTel线程上下文，供其余函数GetSpan
          sendScope = std::make_unique<opentelemetry::trace::Scope>(sendSpan);
        }

        try {
          auto sendResp = co_await msg_client_->SendMessages(sendMsgReq);

          sdkws::SdkWSResp wsResp;
          wsResp.set_requestid(req.requestid());
          wsResp.set_errorcode("0");
          wsResp.set_errormsg("send message success");
          wsResp.set_userid(userId_);
          wsResp.set_type(101);
          wsResp.set_data(sendResp.SerializeAsString());
          respBin = wsResp.SerializeAsString();
          hasResp = true;
        } catch (const std::exception &e) {
          spdlog::error("WSSession: SendMessages failed for user {}: {}",
                        userId_, e.what());

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
        spdlog::warn("WSSession: failed to parse SendMessageReq from user {}",
                     userId_);
      }

      // 统一写回响应
      if (hasResp) {
        ws_.binary(true);
        co_await queuedWrite(
            std::make_shared<std::string>(std::move(respBin)));
      }
      continue;
    }

    // type=106: 统一消息 ACK（在线/离线消息确认，由客户端统一使用）
    if (reqType == 106) {
      co_await handleAck(req);
      continue;
    }

    // 其他业务消息类型暂不做处理，仅记录日志
    spdlog::debug("WSSession: received business message from user {}: type={}, "
                  "requestId={}",
                  userId_, reqType, req.requestid());
  }

  co_await onDisconnect();
}

void WSSession::asyncSend(std::shared_ptr<std::string> payload) {
  // 通过 write_queue_ channel 串行化所有写操作
  auto self = shared_from_this();
  asio::post(ws_.get_executor(), [self, payload]() {
    if (self->write_queue_) {
      self->write_queue_->try_send(boost::system::error_code{}, payload);
    }
  });
}

// ── 写队列：所有 ws_.async_write 统一入口 ──

void WSSession::startWriteQueue() {
  write_queue_ = std::make_unique<asio::experimental::channel<
      void(boost::system::error_code, std::shared_ptr<std::string>)>>(
      ws_.get_executor(), 256);
  asio::co_spawn(
      ws_.get_executor(),
      [self = shared_from_this()]() -> asio::awaitable<void> {
        try {
          co_await self->doDrainWriteQueue();
        } catch (const std::exception &e) {
          spdlog::error("WSSession: write drain crashed for user {}: {}",
                        self->userId_, e.what());
        } catch (...) {
          spdlog::error("WSSession: write drain crashed for user {}",
                        self->userId_);
        }
      },
      asio::detached);
}

asio::awaitable<void> WSSession::doDrainWriteQueue() {
  auto self = shared_from_this();
  boost::system::error_code ec;

  while (true) {
    std::shared_ptr<std::string> payload =
        co_await write_queue_->async_receive(
            asio::redirect_error(asio::use_awaitable, ec));
    if (ec) break; // channel 关闭或出错

    ws_.binary(true);
    co_await ws_.async_write(asio::buffer(*payload),
                             asio::redirect_error(asio::use_awaitable, ec));
    if (ec) {
      spdlog::error("WSSession: write drain error for user {}: {}", userId_,
                    ec.message());
      write_queue_->close();
      break;
    }
  }
}

asio::awaitable<void> WSSession::queuedWrite(
    std::shared_ptr<std::string> payload) {
  if (!write_queue_) {
    co_return;
  }
  if (!write_queue_->try_send(boost::system::error_code{}, payload)) {
    spdlog::warn("WSSession: write queue full for user {}, dropping", userId_);
  }
  co_return;
}

asio::awaitable<void> WSSession::pullAndPushOfflineMsgs() {
  auto self = shared_from_this();
  boost::system::error_code ec;
  constexpr int kBatchSize = 50;
  spdlog::info(
      "WSSession: pulling messages from user_msgs ZSET for user {} (batch_size={})",
      userId_, kBatchSize);

  // TODO: 水位应从 ConversationService.GetConversation.maxSeq 获取
  // （先 Redis 热缓存，miss 则 MongoDB），目前直接读 Redis
  // 1. 从 seq=0 开始拉取，按会话级 last_seq 过滤已 ACK 的消息
  int64_t lastSeq = 0;

  // 2. 循环分段拉取：每批最多 kBatchSize 条，等客户端 ACK 后再拉下一批
  for (;;) {
    // 从 Redis 热存储拉取当前批次
    auto batchMsgs =
        co_await RedisHandler::GetOfflineMsgs(userId_, lastSeq, kBatchSize);

    // Redis 不足时从 MongoDB 冷存储补充（异步，避免阻塞协程线程）
    if (static_cast<int>(batchMsgs.size()) < kBatchSize) {
      int remaining = kBatchSize - static_cast<int>(batchMsgs.size());
      auto coldMsgs = co_await MongoHandler::GetMsgsBySeqFromMongoAsync(
          userId_, lastSeq, remaining);

      // 回温 user_msgs ZSET：MongoDB 冷数据写回 Redis 缓存
      for (auto &msg : coldMsgs) {
        try {
          co_await RedisHandler::SaveOfflineMsg(userId_, msg);
        } catch (const std::exception &e) {
          // 回温失败不影响主流程
        }
        batchMsgs.push_back(std::move(msg));
      }
    }

    // 没有更多消息，结束拉取
    if (batchMsgs.empty()) {
      spdlog::info("WSSession: no more messages for user {}", userId_);
      break;
    }

    spdlog::info(
        "WSSession: batch pulling {} offline messages for user {}, lastSeq={}",
        batchMsgs.size(), userId_, lastSeq);

    // 3. 逐条推送给客户端，按会话级 last_seq 过滤已 ACK 的消息
    int64_t batchMaxSeq = lastSeq;
    for (const auto &msg : batchMsgs) {
      // 检查该会话的 last_seq，跳过已 ACK 的消息
      try {
        std::string seqKey =
            "user:" + userId_ + ":" + msg.convid() + ":last_seq";
        auto val = co_await RedisConnector::instance().get(seqKey);
        int64_t convLastSeq =
            (val && !val->empty()) ? std::stoll(*val) : 0;
        if (msg.seq() <= convLastSeq) {
          spdlog::debug("WSSession: skip already-ACKed msg, conv={}, seq={}, "
                        "lastSeq={}",
                        msg.convid(), msg.seq(), convLastSeq);
          continue;
        }
      } catch (const std::exception &e) {
        // 读失败不阻塞推送
      }

      sdkws::SdkWSResp pushResp;
      pushResp.set_errorcode("0");
      pushResp.set_errormsg("offline message");
      pushResp.set_userid(userId_);
      pushResp.set_type(200); // type 200 = 离线消息推送
      pushResp.set_requestid(msg.servermsgid());
      pushResp.set_data(msg.SerializeAsString());
      std::string pushBin = pushResp.SerializeAsString();

      ws_.binary(true);
      co_await queuedWrite(
          std::make_shared<std::string>(std::move(pushBin)));

      pendingOfflineMsgs_[msg.servermsgid()] = {msg.seq(), msg.convid()};
      if (msg.seq() > batchMaxSeq) {
        batchMaxSeq = msg.seq();
      }
      spdlog::debug(
          "WSSession: pushed offline msg to user {}, msgId={}, seq={}", userId_,
          msg.servermsgid(), msg.seq());
    }

    // 4. 等待客户端 ACK 清空本批次（doReadLoop / handleAck 并发处理并通知）
    //    使用 asio::experimental::channel 实现事件驱动唤醒，替代定时器轮询
    //    capacity=0 保证 rendezvous：handleAck 的 try_send 必须等此处的
    //    async_receive 就绪
    spdlog::info("WSSession: waiting for client ACK on {} msgs for user {}",
                 pendingOfflineMsgs_.size(), userId_);

    // 因为doReadLoop /
    // handleAck这两个是同一个io_context里面执行的，所以只需要使用普通channel就行，不需要使用并发channel
    batch_ack_signal_ = std::make_unique<
        asio::experimental::channel<void(boost::system::error_code)>>(
        ws_.get_executor(), 0);

    // 要先有人等待接收才能发送
    co_await batch_ack_signal_->async_receive(
        asio::redirect_error(asio::use_awaitable, ec));

    batch_ack_signal_.reset();

    // 连接断开时 onDisconnect 会清理 pendingOfflineMsgs_ 并 cancel channel
    if (ec) {
      spdlog::info("WSSession: ACK wait cancelled (disconnect), user {}",
                   userId_);
      co_return;
    }

    spdlog::info("WSSession: batch ACK done for user {}, batchMaxSeq={}",
                 userId_, batchMaxSeq);

    // 5. 推进 lastSeq 到本批次最大值，继续下一批
    lastSeq = batchMaxSeq;
  }

  spdlog::info("WSSession: offline push complete for user {}", userId_);
}

boost::asio::awaitable<void> WSSession::handleAck(const sdkws::SdkWSReq &req) {
  // 解析 ACK 请求
  sdkws::AckReq ackReq;
  if (!ackReq.ParseFromString(req.data())) {
    spdlog::warn("WSSession: failed to parse AckReq from user {}", userId_);
    co_return;
  }

  spdlog::info("WSSession: unified ACK from user {}, {} msgIds, ackType={}",
               userId_, ackReq.servermsgids_size(), ackReq.acktype());

  // 遍历所有已确认的 serverMsgIDs
  for (int i = 0; i < ackReq.servermsgids_size(); ++i) {
    std::string serverMsgID = ackReq.servermsgids(i);

    // 检查是否是重连拉取的离线消息 ACK（在 pendingOfflineMsgs_ 中）
    auto it = pendingOfflineMsgs_.find(serverMsgID);
    if (it != pendingOfflineMsgs_.end()) {
      int64_t seq = it->second.seq;

      // Redis ACK 清理
      co_await RedisHandler::AckOfflineMsg(userId_, serverMsgID);

      // TODO: 水位应由 ConversationService.SetConversationMaxSeq 管理
      // （双写 Redis + MongoDB conversations.maxSeq），目前只写 Redis
      // 更新会话级 last_seq
      std::string convID = it->second.convID;
      try {
        std::string seqKey =
            "user:" + userId_ + ":" + convID + ":last_seq";
        auto oldVal = co_await RedisConnector::instance().get(seqKey);
        int64_t oldSeq = (oldVal && !oldVal->empty()) ? std::stoll(*oldVal) : 0;
        if (seq > oldSeq) {
          co_await RedisConnector::instance().set(seqKey,
                                                   std::to_string(seq));
        }
      } catch (const std::exception &e) {
        spdlog::error(
            "WSSession: failed to update last_seq on ACK for user={}, "
            "conv={}: {}",
            userId_, convID, e.what());
      }

      pendingOfflineMsgs_.erase(it);
      spdlog::debug("WSSession: offline msg ACKed via type 106, msgId={}, "
                    "seq={}, remaining={}",
                    serverMsgID, seq, pendingOfflineMsgs_.size());

      // 本批次全部 ACK 完成，通知 pullAndPushOfflineMsgs 继续拉取下一批
      if (pendingOfflineMsgs_.empty() && batch_ack_signal_) {
        batch_ack_signal_->try_send(boost::system::error_code{});
      }
    }

    // 通过 gRPC 转发 ACK 给 PushService（处理在线/离线消息的最终确认）
    try {
      ::push::AckMsgReq ackMsgReq;
      ackMsgReq.set_userid(userId_);
      ackMsgReq.set_servermsgid(serverMsgID);
      ackMsgReq.set_seq(0); // seq 未知时填0，PushService 自行查 Redis
      ackMsgReq.set_acktype(ackReq.acktype());
      co_await push_client_->AckMsg(ackMsgReq);
    } catch (const std::exception &e) {
      spdlog::error("WSSession: AckMsg gRPC failed for user {}, msgId={}: {}",
                    userId_, serverMsgID, e.what());
    }
  }
}

boost::asio::awaitable<void> WSSession::onDisconnect() {
  if (userId_.empty())
    co_return;

  // 唤醒 pullAndPushOfflineMsgs 中等待 ACK 的协程（避免泄漏悬挂协程）
  if (batch_ack_signal_) {
    batch_ack_signal_->cancel();
    batch_ack_signal_.reset();
  }

  bool isActive = WSSessionManager::instance().unregisterSession(
      userId_, shared_from_this());
  if (!isActive)
    co_return; // 旧 session，不动 Redis 和全局状态

  // 只有仍然是活跃 session 才设离线
  try {
    co_await RedisConnector::instance().setex("user:" + userId_ + ":online",
                                              300, "0");
  } catch (const std::exception &e) {
    spdlog::error("WSSession: Redis offline update error: {}", e.what());
  }

  // 3. 尝试优雅关闭 WebSocket
  boost::system::error_code ec;
  co_await ws_.async_close(websocket::close_code::normal,
                           asio::redirect_error(asio::use_awaitable, ec));
  if (ec) {
    spdlog::debug("WSSession: graceful close error: {}", ec.message());
  }
}
