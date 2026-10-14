#include "gateway/ws_session.h"
#include "apeauth.pb.h"
#include "gateway/ws_session_manager.h"
#include "groupchat.pb.h"
#include "msg.pb.h"
#include "om/otel_tracer.h"
#include "push.pb.h"
#include "sdkws.pb.h"
#include "services/auth_service/client.h"
#include "services/group_service/client.h"
#include "services/friend_service/client.h"
#include "services/conversation_service/client.h"
#include "services/msg_service/client.h"
#include "services/push_service/client.h"
#include "storage/mongohandler.h"
#include "storage/mysqlhandler.h"
#include "storage/redisconnector.h"
#include "util/message_validation.h"
#include <cstdint>
#include <opentelemetry/trace/scope.h>


#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <spdlog/spdlog.h>
#include <string>

namespace asio = boost::asio;

WSSession::WSSession(asio::ssl::stream<tcp::socket> &&stream,
                     AuthClient *auth_client, MsgClient *msg_client,
                     PushClient *push_client, GroupClient *group_client, FriendClient *friend_client,
                     ConversationClient *conversation_client)
    : ws_(std::move(stream)), auth_client_(auth_client),
      msg_client_(msg_client), push_client_(push_client),
      group_client_(group_client), friend_client_(friend_client),
      conversation_client_(conversation_client) {}

WSSession::~WSSession() = default;

boost::asio::awaitable<void> WSSession::start() {
  // 使用 lambda 启动协程，以便捕获 shared_from_this()
  auto self = shared_from_this();
  asio::co_spawn(
      ws_.get_executor(),
      [self]() -> asio::awaitable<void> {
        boost::system::error_code ec;

        // 1. 先 TLS 握手
        co_await self->ws_.next_layer().async_handshake(
            asio::ssl::stream_base::server,
            asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
          spdlog::error("WSSession: TLS handshake failed: {}", ec.message());
          co_return;
        }

        // 2. 再WebSocket 握手
        co_await self->ws_.async_accept(
            asio::redirect_error(asio::use_awaitable, ec));

        if (ec) {
          spdlog::error("WSSession: WebSocket handshake failed: {}",
                        ec.message());
          co_return;
        }

        spdlog::info("WSSession: WebSocket connection accepted from {}",
                     self->ws_.next_layer()
                         .next_layer()
                         .remote_endpoint()
                         .address()
                         .to_string());

        // 2. 读取帧（可能是注册或者登陆）
        bool failed = false;
        try { co_await self->doRead(); }
        catch (const std::exception& e) {
          failed = true;
          spdlog::error("WSSession: session failed: {}", e.what());
        }
        // handleAuth 正常返回时读循环仍在运行，不能关闭已认证连接。
        if (failed) co_await self->onDisconnect();
      },
      asio::detached);
  co_return;
}

boost::asio::awaitable<void> WSSession::handleReg(sdkws::SdkWSReq &req) {
  auto self = shared_from_this();
  boost::system::error_code ec;

  // data字段放信息
  std::string regStr = req.data();
  ::auth::RegisterRequest regReq;
  if (!regReq.ParseFromString(regStr)) {
    spdlog::warn("WSSession: invalid register message (Parse error occurred), "
                 "raw_size={}",
                 regStr.size());

    // 发送错误响应（Protobuf 二进制）
    sdkws::SdkWSResp errResp;
    errResp.set_errorcode("PARSE_ERROR");
    errResp.set_errormsg("Parse error occurred");
    errResp.set_requestid(req.requestid());
    errResp.set_type(req.type());    // 对应注册账号req的类型
    std::string errBin = errResp.SerializeAsString();

    ws_.binary(true);
    co_await ws_.async_write(asio::buffer(errBin),
                             asio::redirect_error(asio::use_awaitable, ec));
    co_await ws_.async_close(websocket::close_code::policy_error,
                             asio::redirect_error(asio::use_awaitable, ec));
    co_return;
  }

  if (regReq.phone().empty() || regReq.nickname().empty() ||
      regReq.password().empty()) {
    spdlog::warn(
        "WSSession: invalid register message (register info imcomplete)");

    // 发送错误响应（Protobuf 二进制）
    sdkws::SdkWSResp errResp;
    errResp.set_errorcode("IMCOMPLETE_INFO");
    errResp.set_errormsg("Register info imcomplete");
    errResp.set_requestid(req.requestid());
    errResp.set_type(req.type());
    std::string errBin = errResp.SerializeAsString();

    ws_.binary(true);
    co_await ws_.async_write(asio::buffer(errBin),
                             asio::redirect_error(asio::use_awaitable, ec));
    co_await ws_.async_close(websocket::close_code::policy_error,
                             asio::redirect_error(asio::use_awaitable, ec));
    co_return;
  }

  // 注册
  auth::AuthRegResponse resp;
  try {
    resp = co_await auth_client_->RegisterRequest(
        regReq.phone(), regReq.nickname(), regReq.password(), req.requestid());
    if (resp.success()) {
      spdlog::info("WSSession: AuthService Register success, account = {}", resp.user().account());
    } else {
      spdlog::warn("WSSession: AuthService register failed for phone = {}", regReq.phone());
    }
  } catch (const std::exception &e) {
    spdlog::error("WSSession: AuthService Register error: {}", e.what());
    resp.set_success(false);
    resp.set_errorcode("REG_CLIENT_ERROR");
    resp.set_errormessage("Registration outcome unknown; retry the same request ID");
  }

  sdkws::SdkWSResp wsResp;
  wsResp.set_type(0);
  wsResp.set_requestid(req.requestid());
  wsResp.set_deviceid(req.deviceid());

  if (resp.success()) {
    wsResp.set_errorcode("");
    wsResp.set_errormsg("");
    wsResp.set_account(resp.user().account());
  } else {
    wsResp.set_errorcode(resp.errorcode().empty() ? "REGISTER_FAILED" : resp.errorcode());
    wsResp.set_errormsg(resp.errormessage().empty() ? "Registration failed" : resp.errormessage());
  }

  std::string binary = wsResp.SerializeAsString();
  ws_.binary(true);
  co_await ws_.async_write(asio::buffer(binary));

  co_return;
}

boost::asio::awaitable<void> WSSession::handleAuth(sdkws::SdkWSReq &req) {
  auto self = shared_from_this();
  boost::system::error_code ec;

  if (req.account() == 0) {
    spdlog::warn("WSSession: invalid auth protobuf message (missing account)");

    // 发送错误响应（Protobuf 二进制）
    sdkws::SdkWSResp errResp;
    errResp.set_errorcode("INVALID_PARAM");
    errResp.set_errormsg("missing account");
    errResp.set_type(0); // error type
    std::string errBin = errResp.SerializeAsString();

    ws_.binary(true);
    co_await ws_.async_write(asio::buffer(errBin),
                             asio::redirect_error(asio::use_awaitable, ec));
    co_await ws_.async_close(websocket::close_code::policy_error,
                             asio::redirect_error(asio::use_awaitable, ec));
    co_return;
  }

  const uint64_t account_ = req.account();
  const std::string plainPWD_ = req.data();
  std::string token_ = req.token();

  spdlog::info(
      "WSSession: auth request login from user={}, reqType={}, trackID={}",
      account_, req.type(), req.trackid());

  bool token_valid = false;

  // ── 分支 1：token 为空，走密码登录 ──
  if (token_.empty()) {
    auto resp = co_await auth_client_->LoginRequest(account_, plainPWD_);
    if (!resp.success()) {
      spdlog::warn("WSSession: LoginRequest failed for account={}, reason={}",
                   account_, resp.errormessage());

      // 必须发错误帧再关，否则客户端只看到 close(1000) 什么都收不到
      sdkws::SdkWSResp errResp;
      errResp.set_errorcode("401");
      errResp.set_errormsg("login failed: " + resp.errormessage());
      errResp.set_account(account_);
      errResp.set_type(0);
      errResp.set_requestid(req.requestid());
      std::string errBin = errResp.SerializeAsString();

      ws_.binary(true);
      co_await ws_.async_write(asio::buffer(errBin),
                               asio::redirect_error(asio::use_awaitable, ec));
      co_await ws_.async_close(websocket::close_code::policy_error,
                               asio::redirect_error(asio::use_awaitable, ec));
      co_return;
    }

    // 登录成功，保存 token 供回传与后续复用
    token_ = resp.token();
    token_valid = true;

    spdlog::info("WSSession: LoginRequest ok for account={}, token issued",
                 account_);
  }
  // ── 分支 2：token 非空，走 token 校验 ──
  else {
    try {
      auto resp = co_await auth_client_->ValidateToken(token_);
      if (resp.valid() && resp.account() == account_) {
        token_valid = true;
        spdlog::info("WSSession: token verified via AuthService for userID {} "
                     "(account: {})",
                     resp.userid(), std::to_string(account_));
      } else {
        spdlog::warn("WSSession: AuthService token validation failed for "
                     "account={}",
                     account_);
      }
    } catch (const std::exception &e) {
      spdlog::error("WSSession: AuthService ValidateToken error: {}", e.what());
    }
  }

  // ── 认证失败统一处理 ──
  if (!token_valid) {
    spdlog::warn(
        "WSSession: auth failed for account {} - invalid or expired token",
        account_);

    sdkws::SdkWSResp errResp;
    errResp.set_errorcode("INVALID_PARAM");
    errResp.set_errormsg("invalid or expired token");
    errResp.set_account(account_);
    errResp.set_type(0);
    errResp.set_requestid(req.requestid());
    std::string errBin = errResp.SerializeAsString();

    ws_.binary(true);
    co_await ws_.async_write(asio::buffer(errBin),
                             asio::redirect_error(asio::use_awaitable, ec));
    co_await ws_.async_close(websocket::close_code::policy_error,
                             asio::redirect_error(asio::use_awaitable, ec));
    co_return;
  }

  // ── 认证成功 ──
  this->account_ = account_;
  spdlog::info("WSSession: user {} authenticated", account_);

  // 注册到 SessionManager
  WSSessionManager::instance().registerSession(account_, self);

  // 启动写队列（此后所有 ws_.async_write 统一走 queuedWrite）
  startWriteQueue();

  // 更新 Redis 在线状态
  try {
    co_await RedisConnector::instance().setex(
        "user:" + std::to_string(account_) + ":online", 300, "1");
  } catch (const std::exception &e) {
    spdlog::error("WSSession: failed to update Redis online status: {}",
                  e.what());
  }

  // 发送认证成功响应（Protobuf 二进制）
  sdkws::SdkWSResp okResp;
  okResp.set_errorcode("");
  okResp.set_errormsg("");
  okResp.set_account(account_);
  okResp.set_type(req.type()); 
  okResp.set_requestid(req.requestid());
  okResp.set_token(token_);
  okResp.set_deviceid(req.deviceid());
  std::string okBin = okResp.SerializeAsString();

  ws_.binary(true);
  co_await queuedWrite(std::make_shared<std::string>(std::move(okBin)));

  // 启动消息读循环
  asio::co_spawn(
      ws_.get_executor(),
      [self]() -> asio::awaitable<void> {
        try {
          co_await self->doReadLoop();
        } catch (const std::exception &e) {
          spdlog::error("doReadLoop exception, account={}: {}", self->account_,
                        e.what());
        } catch (...) {
          spdlog::error("doReadLoop unknown exception, account={}",
                        self->account_);
        }
        co_await self->onDisconnect();
      },
      asio::detached);

  // 分段拉取并推送离线消息
  std::string syncError;
  try {
    co_await pullAndPushOfflineMsgs();
  } catch (const std::exception &e) {
    spdlog::error("WSSession: pullAndPushOfflineMsgs error for user {}: {}",
                  account_, e.what());
    syncError = e.what();
  }
  if (disconnected_) co_return;
  if (!syncError.empty()) {
    sdkws::SdkWSResp failed;
    failed.set_type(209);
    failed.set_account(account_);
    failed.set_errorcode("SYNC_ERROR");
    failed.set_errormsg("Offline message synchronization failed; sign in again to retry");
    co_await queuedWrite(std::make_shared<std::string>(failed.SerializeAsString()));
  }

  co_return;
}

boost::asio::awaitable<void> WSSession::fallback(sdkws::SdkWSReq &req) {
  auto self = shared_from_this();
  boost::system::error_code ec;
  try {
    spdlog::warn("WSSession: message type unsupported!");
    sdkws::SdkWSResp errResp;
    errResp.set_errorcode("4401");
    errResp.set_errormsg("message type unsupported");
    errResp.set_type(0);
    errResp.set_requestid(req.requestid());
    std::string errBin = errResp.SerializeAsString();
    ws_.binary(true);
    co_await ws_.async_write(asio::buffer(errBin),
                             asio::redirect_error(asio::use_awaitable, ec));
    if (ec) {
      spdlog::error("fallback send resp failed: {}", ec.message());
      co_return;
    }
    co_await ws_.async_close(websocket::close_code(4404),
                             asio::redirect_error(asio::use_awaitable, ec));
  } catch (const std::exception &e) {
    spdlog::error("fallback exception: {}", e.what());
  } catch (...) {
    spdlog::error("fallback unknown exception");
  }
  co_return;
}

asio::awaitable<void> WSSession::doRead() {
  auto self = shared_from_this();
  boost::system::error_code ec;

  // 设置合理的超时
  ws_.set_option(
      websocket::stream_base::timeout::suggested(beast::role_type::server));

  co_await ws_.async_read(buffer_,
                          asio::redirect_error(asio::use_awaitable, ec));

  if (ec) {
    spdlog::error("WSSession: first read failed: {}", ec.message());
    co_return;
  }

  // 解析认证 Protobuf 二进制帧
  std::string reqData = beast::buffers_to_string(buffer_.data());
  buffer_.consume(buffer_.size());

  // 尝试解析 sdkws::SdkWSReq
  sdkws::SdkWSReq sdkReq;
  if (!sdkReq.ParseFromString(reqData)) {
    spdlog::warn("WSSession: invalid protobuf message (Parse error occurred), "
                 "raw_size={}",
                 reqData.size());

    // 发送错误响应（Protobuf 二进制）
    sdkws::SdkWSResp errResp;
    errResp.set_errorcode("1");
    errResp.set_errormsg("Parse error occurred");
    errResp.set_type(0); // error type
    std::string errBin = errResp.SerializeAsString();

    ws_.binary(true);
    co_await ws_.async_write(asio::buffer(errBin),
                             asio::redirect_error(asio::use_awaitable, ec));
    co_await ws_.async_close(websocket::close_code::policy_error,
                             asio::redirect_error(asio::use_awaitable, ec));
    co_return;
  }

  switch (sdkReq.type()) {
  case 100:
    co_await handleReg(sdkReq);
    break;
  case 101:
    co_await handleAuth(sdkReq);
    break;
  default:
    co_await fallback(sdkReq);
    break;
  }
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
        spdlog::info("WSSession: user {} connection closed", account_);
      } else {
        spdlog::error("WSSession: read error for user {}: {}, category={}, code={}",
                      account_, ec.message(), ec.category().name(), ec.value());
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
          account_, msgData.size());
      continue;
    }

    int32_t reqType = req.type();
    spdlog::debug("WSSession: received req from user {}: type={}, requestId={}",
                  account_, reqType, req.requestid());

    // 处理心跳 ping（type=0 约定为 ping）
    if (reqType == 0) {
      sdkws::SdkWSResp pongResp;
      pongResp.set_requestid(req.requestid());
      pongResp.set_errorcode("0");
      pongResp.set_errormsg("pong");
      pongResp.set_account(account_);
      pongResp.set_type(0);
      std::string pongBin = pongResp.SerializeAsString();

      ws_.binary(true);
      co_await queuedWrite(std::make_shared<std::string>(std::move(pongBin)));

      // 心跳续期 Redis 在线状态
      try {
        co_await RedisConnector::instance().setex(
            "user:" + std::to_string(account_) + ":online", 300,
            "1"); // 5分钟过期（靠心跳续期）
      } catch (const std::exception &e) {
        spdlog::error("WSSession: Redis heartbeat update error: {}", e.what());
      }
      continue;
    }

    // type=101: 单聊消息
    if (reqType == 101) {
      sdkws::SendMessageReq sendMsgReq;
      std::string respBin;
      bool hasResp = false;

      if (sendMsgReq.ParseFromString(req.data())) {
        for (auto &message : *sendMsgReq.mutable_msgs())
          message.set_sendid(account_);

        // ── OTel: 创建 Root Span,作为整条 Trace 的起点 ──
        auto tracer = ape::otel::GetTracer();
        opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> sendSpan;
        std::unique_ptr<opentelemetry::trace::Scope> sendScope;
        if (tracer) {
          sendSpan = tracer->StartSpan(
              "WS /msg/send", {{"ws.message_type", "send_msg"},
                               {"ws.user_id", account_},
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
          wsResp.set_account(account_);
          wsResp.set_type(101);
          wsResp.set_data(sendResp.SerializeAsString());
          respBin = wsResp.SerializeAsString();
          hasResp = true;
        } catch (const std::exception &e) {
          spdlog::error("WSSession: SendMessages failed for user {}: {}",
                        account_, e.what());

          sdkws::SdkWSResp errResp;
          errResp.set_requestid(req.requestid());
          errResp.set_errorcode("500");
          errResp.set_errormsg(std::string("SendMessages failed: ") + e.what());
          errResp.set_account(account_);
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
                     account_);
      }

      // 统一写回响应
      if (hasResp) {
        ws_.binary(true);
        co_await queuedWrite(std::make_shared<std::string>(std::move(respBin)));
      }
      continue;
    }

    // type=106: 统一消息 ACK（在线/离线消息确认，由客户端统一使用）
    if (reqType == 106) {
      co_await handleAck(req);
      continue;
    }

    // 群消息
    if (reqType == 250) {
      groupchat::Command command;
      groupchat::Reply reply;
      if (!command.ParseFromString(req.data())) {
        reply.set_errorcode("INVALID_PARAM");
        reply.set_errormessage("Invalid group request");
      } else {
        command.set_actor(account_);
        if(group_client_) reply=co_await group_client_->Execute(command);
        else { reply.set_errorcode("DB_ERROR"); reply.set_errormessage("Group service unavailable"); }
      }
      sdkws::SdkWSResp response;
      response.set_type(250); response.set_account(account_);
      response.set_requestid(req.requestid()); response.set_data(reply.SerializeAsString());
      ws_.binary(true);
      co_await queuedWrite(std::make_shared<std::string>(response.SerializeAsString()));
      continue;
    }

    // 拉取历史消息
    if (reqType == 244) {
      sdkws::SdkWSResp response;
      response.set_type(244);
      response.set_requestid(req.requestid());
      response.set_account(account_);
      response.set_errorcode("0");
      msg::GetSingleChatHistoryReq command;
      if (!command.ParseFromString(req.data()) || !command.peeruserid()) {
        response.set_errorcode("INVALID_PARAM");
        response.set_errormsg("Invalid history request");
      } else {
        command.set_owneruserid(account_);
        try {
          auto result = co_await msg_client_->GetSingleChatHistory(command);
          response.set_data(result.SerializeAsString());
        } catch (const std::exception& e) {
          response.set_errorcode("HISTORY_ERROR");
          response.set_errormsg(e.what());
        }
      }
      co_await queuedWrite(std::make_shared<std::string>(response.SerializeAsString()));
      continue;
    }

    // 会话相关
    if (reqType >= 240 && reqType <= 243) {
      sdkws::SdkWSResp response;
      response.set_type(reqType);
      response.set_requestid(req.requestid());
      response.set_account(account_);
      response.set_errorcode("0");
      try {
        if (!conversation_client_)
          throw std::runtime_error("Conversation service unavailable");
        if (reqType == 240) {                                               // 获取全部会话
          auto result = co_await conversation_client_->List(account_);
          response.set_data(result.SerializeAsString());
        } else if (reqType == 241) {
          conversation::MarkConversationReadReq command;
          if (!command.ParseFromString(req.data()) || command.conversationid().empty())
            throw std::invalid_argument("Invalid read request");
          command.set_owneruserid(account_);
          co_await conversation_client_->MarkRead(command);
        } else if (reqType == 242) {
          conversation::UpdateConversationSettingsReq command;
          if (!command.ParseFromString(req.data()) || command.conversationid().empty())
            throw std::invalid_argument("Invalid settings request");
          command.set_owneruserid(account_);
          co_await conversation_client_->UpdateSettings(command);
        } else {
          conversation::DeleteConversationReq command;
          if (!command.ParseFromString(req.data()) || command.conversationid().empty())
            throw std::invalid_argument("Invalid delete request");
          command.set_owneruserid(account_);
          co_await conversation_client_->Delete(command);
        }
      } catch (const std::exception& e) {
        response.set_errorcode("CONVERSATION_ERROR");
        response.set_errormsg(e.what());
      }
      co_await queuedWrite(std::make_shared<std::string>(response.SerializeAsString()));
      continue;
    }

    // 好友服务相关
    if (reqType >= 220 && reqType <= 225) {
      friendpb::Reply reply;
      friendpb::Command command;
      if (!command.ParseFromString(req.data())) {
        reply.set_errorcode("INVALID_PARAM");
        reply.set_errormessage("Invalid friend request");
      } else if (!friend_client_) {
        reply.set_errorcode("SERVICE_UNAVAILABLE");
        reply.set_errormessage("Friend service unavailable");
      } else {
        command.set_actor(account_);
        command.set_operation(static_cast<friendpb::Operation>(reqType - 220));
        try {
          reply = co_await friend_client_->Execute(command);
        } catch (const std::exception& e) {
          spdlog::error("Friend request failed: {}", e.what());
          reply.set_errorcode("SERVICE_UNAVAILABLE");
          reply.set_errormessage("Friend service unavailable");
        }
      }
      sdkws::SdkWSResp response;
      response.set_type(reqType);
      response.set_requestid(req.requestid());
      response.set_account(account_);
      response.set_errorcode(reply.errorcode());
      response.set_errormsg(reply.errormessage());
      response.set_data(reply.SerializeAsString());
      co_await queuedWrite(std::make_shared<std::string>(response.SerializeAsString()));
      if (reply.errorcode() == "0") {
        for (const auto account : reply.changedaccounts()) {
          if (!WSSessionManager::instance().hasSession(account)) {
            spdlog::debug("Friend notification deferred to login sync, user={}", account);
            continue;
          }
          sdkws::SdkWSResp notification;
          notification.set_type(230);
          notification.set_account(account);
          notification.set_errorcode("0");
          WSSessionManager::instance().pushToUser(account,
              std::make_shared<std::string>(notification.SerializeAsString()));
        }
      }
      continue;
    }

    // 其他业务消息类型暂不做处理，仅记录日志
    spdlog::debug("WSSession: received business message from user {}: type={}, "
                  "requestId={}",
                  account_, reqType, req.requestid());
  }

  co_await onDisconnect();
}

void WSSession::asyncSend(std::shared_ptr<std::string> payload) {
  // 通过 write_queue_ channel 串行化所有写操作
  auto self = shared_from_this();
  asio::post(ws_.get_executor(), [self, payload]() {
    if (self->disconnected_) return;
    if (!self->write_queue_ || !self->write_queue_->try_send(boost::system::error_code{}, payload)) {
      // 异步推送无法向调用方返回队列失败，关闭连接让未 ACK 消息重登恢复。
      asio::co_spawn(self->ws_.get_executor(), [self]() -> asio::awaitable<void> {
        co_await self->onDisconnect();
      }, asio::detached);
    }
  });
}

// ── 写队列：所有 ws_.async_write 统一入口 ──

void WSSession::startWriteQueue() {
  write_queue_ = std::make_unique<asio::experimental::channel<void(
      boost::system::error_code, std::shared_ptr<std::string>)>>(
      ws_.get_executor(), 256);
  asio::co_spawn(
      ws_.get_executor(),
      [self = shared_from_this()]() -> asio::awaitable<void> {
        try {
          co_await self->doDrainWriteQueue();
        } catch (const std::exception &e) {
          spdlog::error("WSSession: write drain crashed for user {}: {}",
                        self->account_, e.what());
        } catch (...) {
          spdlog::error("WSSession: write drain crashed for user {}",
                        self->account_);
        }
        co_await self->onDisconnect();
      },
      asio::detached);
}

asio::awaitable<void> WSSession::doDrainWriteQueue() {
  auto self = shared_from_this();
  boost::system::error_code ec;

  while (!disconnected_) {
    std::shared_ptr<std::string> payload = co_await write_queue_->async_receive(
        asio::redirect_error(asio::use_awaitable, ec));
    if (ec)
      break; // channel 关闭或出错

    ws_.binary(true);
    co_await ws_.async_write(asio::buffer(*payload),
                             asio::redirect_error(asio::use_awaitable, ec));
    if (ec) {
      spdlog::error("WSSession: write drain error for user {}: {}", account_,
                    ec.message());
      co_await onDisconnect();
      break;
    }
  }
}

asio::awaitable<void>
WSSession::queuedWrite(std::shared_ptr<std::string> payload) {
  if (disconnected_ || !write_queue_ ||
      !write_queue_->try_send(boost::system::error_code{}, payload)) {
    // 入队失败不是发送成功；停止本连接，不能登记后无限等待不存在的 ACK。
    co_await onDisconnect();
    throw boost::system::system_error(asio::error::operation_aborted);
  }
  co_return;
}

asio::awaitable<void> WSSession::pullAndPushOfflineMsgs() {
  auto self = shared_from_this();
  // Mongo 正文是恢复候选消息的唯一分页来源。Redis 离线 ZSET 仍用于其他
  // 投递/清理流程，但不能与 Mongo 分页拼接后取最大序号，否则会跳过缓存缺口。
  co_await scanOfflineMessages(
      [this](int64_t after, int limit) -> asio::awaitable<std::vector<sdkws::MsgData>> {
        co_return co_await MongoHandler::GetMsgsBySeqFromMongoAsync(account_, after, limit);
      },
      [this](sdkws::MsgData message) -> asio::awaitable<void> {
        co_await deliverOfflineMessage(std::move(message));
      });
  if (disconnected_) co_return;

  // 只有正常读到末尾且所有下发消息的 ACK 已持久化，才通知本轮同步完成。
  sdkws::SdkWSResp complete;
  complete.set_type(209); complete.set_account(account_); complete.set_errorcode("0");
  complete.set_errormsg("offline sync complete");
  co_await queuedWrite(std::make_shared<std::string>(complete.SerializeAsString()));
  spdlog::info("WSSession: offline sync complete, user={}", account_);
}

asio::awaitable<void> WSSession::scanOfflineMessages(
    std::function<asio::awaitable<std::vector<sdkws::MsgData>>(int64_t,int)> fetch,
    std::function<asio::awaitable<void>(sdkws::MsgData)> deliver) {
  constexpr int batchSize = 50;
  // 这是本轮扫描游标，不是永久 ACK 水位。重登从 0 查询未确认候选消息，
  // 逐条 MySQL 回执去重；不能用最大 ACK 跳过乱序 ACK 前仍未接收的消息。
  int64_t after = 0;
  while (!disconnected_) {
    auto messages = co_await fetch(after, batchSize);
    if (disconnected_) co_return;
    if (messages.empty()) co_return;
    if (messages.size() > batchSize) throw std::runtime_error("Offline batch exceeds limit");
    int64_t next = after;
    for (auto& message : messages) {
      if (disconnected_) co_return;
      // 单一来源必须严格升序。错误收件人、旧群消息或倒退游标不能静默放行。
      if (!message_validation::IsSingleMessageFor(account_,message) || message.seq() <= next)
        throw std::runtime_error("Invalid offline candidate or sequence order");
      next = message.seq();
      co_await deliver(std::move(message));
    }
    // 有效消息等待持久化 ACK；全部已 ACK/失效的候选也可推进扫描游标。
    // waitOfflineBatch 同时处理提前 ACK、断线和 60 秒超时，无忙等轮询。
    co_await waitOfflineBatch();
    if (disconnected_) co_return;
    after = next;
  }
}

asio::awaitable<void> WSSession::deliverOfflineMessage(sdkws::MsgData message) {
  // Mongo status 更新可能落后于 ACK：下发前仍要在账号对锁下检查好友关系、
  // 删除水位、正文提交阶段以及 MySQL 持久化回执。已确认消息不重复下发。
  co_await MySQLHandler::WithFriendMessage(message,
      [this, &message]() -> asio::awaitable<void> {
        co_await asio::co_spawn(ws_.get_executor(), [this, &message]() -> asio::awaitable<void> {
          if (disconnected_) throw boost::system::system_error(asio::error::operation_aborted);
          sdkws::SdkWSResp frame;
          frame.set_type(200); frame.set_account(account_); frame.set_errorcode("0");
          frame.set_errormsg("offline message"); frame.set_requestid(message.servermsgid());
          frame.set_data(message.SerializeAsString());
          // 先登记再入队，避免快速 ACK 到达时找不到待确认项。
          // queuedWrite 成功仅代表入队；写失败/队列满会关闭连接，供重登恢复。
          pendingOfflineMsgs_[message.servermsgid()] = {message.seq(),message.convid()};
          co_await queuedWrite(std::make_shared<std::string>(frame.SerializeAsString()));
          spdlog::debug("WSSession: offline message queued, user={}, message={}, seq={}",
                       account_,message.servermsgid(),message.seq());
        }, asio::use_awaitable);
      }, true);
}

asio::awaitable<void> WSSession::waitOfflineBatch(std::chrono::steady_clock::duration timeout) {
  if (disconnected_ || pendingOfflineMsgs_.empty()) co_return;
  auto timer = std::make_shared<asio::steady_timer>(ws_.get_executor());
  batch_ack_timer_ = timer;
  timer->expires_after(timeout);
  boost::system::error_code ec;
  co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
  if (batch_ack_timer_ == timer) batch_ack_timer_.reset();
  if (disconnected_ || pendingOfflineMsgs_.empty()) co_return;
  // 到期或异常取消但仍有待确认消息：关闭会话，保持数据库回执不变供重登恢复。
  spdlog::warn("WSSession: offline ACK wait failed/timed out, user={}, pending={}", account_, pendingOfflineMsgs_.size());
  co_await onDisconnect();
}

boost::asio::awaitable<void> WSSession::handleAck(const sdkws::SdkWSReq &req) {
  sdkws::AckReq ackReq;
  if (!ackReq.ParseFromString(req.data()) || (ackReq.acktype() != 1 && ackReq.acktype() != 2))
    co_return;
  for (const auto &serverMsgID : ackReq.servermsgids()) {
    bool accepted = false;
    for (int attempt = 0; attempt < 2 && !accepted; ++attempt) {
      try {
        push::AckMsgReq request;
        request.set_userid(account_);
        request.set_servermsgid(serverMsgID);
        request.set_acktype(ackReq.acktype());
        co_await push_client_->AckMsg(request);
        accepted = true;
      } catch (const std::exception &e) {
        spdlog::error("WSSession: ACK not persisted, user={}, message={}: {}", account_, serverMsgID, e.what());
      }
      if (!accepted && attempt == 0) {
        asio::steady_timer timer(ws_.get_executor());
        timer.expires_after(std::chrono::milliseconds(200));
        co_await timer.async_wait(asio::use_awaitable);
      }
    }
    if (!accepted) {
      boost::system::error_code ec;
      beast::get_lowest_layer(ws_).close(ec);
      co_return;
    }
    auto found = pendingOfflineMsgs_.find(serverMsgID);
    if (found != pendingOfflineMsgs_.end()) {
      pendingOfflineMsgs_.erase(found);
      if (pendingOfflineMsgs_.empty() && batch_ack_timer_)
        batch_ack_timer_->cancel();
    }
  }
}

boost::asio::awaitable<void> WSSession::onDisconnect() {
  if (disconnected_) co_return;
  disconnected_ = true;
  // 先同步停止所有本地等待，再访问 Redis；旧会话也必须释放自身资源。
  if (batch_ack_timer_) batch_ack_timer_->cancel();
  pendingOfflineMsgs_.clear();
  if (write_queue_) { write_queue_->cancel(); write_queue_->close(); }
  boost::system::error_code closeError;
  auto& socket = beast::get_lowest_layer(ws_);
  socket.cancel(closeError);
  socket.shutdown(tcp::socket::shutdown_both, closeError);
  socket.close(closeError);
  if (account_ == 0) co_return;

  bool isActive = WSSessionManager::instance().unregisterSession(
      account_, shared_from_this());
  if (!isActive)
    co_return; 

  // 只有仍然是活跃 session 才设离线
  try {
    co_await RedisConnector::instance().setex(
        "user:" + std::to_string(account_) + ":online", 300, "0");
  } catch (const std::exception &e) {
    spdlog::error("WSSession: Redis offline update error: {}", e.what());
  }

  // 已关闭底层 socket，不与在途 async_write 并发启动 WebSocket close。
}
