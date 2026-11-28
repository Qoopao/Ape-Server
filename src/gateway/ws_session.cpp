#include "gateway/ws_session.h"
#include "apeauth.pb.h"
#include "conversation.pb.h"
#include "services/gateway_router.h"
#include "util/gateway_route.h"
#include "gateway/ws_session_manager.h"
#include "groupchat.pb.h"
#include "msg.pb.h"
#include "om/obs_log.h"
#include "om/obs_metrics.h"
#include "om/trace_context.h"
#include "push.pb.h"
#include "sdkws.pb.h"
#include "services/auth_service/client.h"
#include "services/conversation_service/client.h"
#include "services/friend_service/client.h"
#include "services/group_service/client.h"
#include "services/msg_service/client.h"
#include "services/push_service/client.h"
#include "storage/mongohandler.h"
#include "storage/mysqlhandler.h"
#include "storage/redisconnector.h"
#include "util/message_validation.h"
#include "util/uuid.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <map>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>


namespace asio = boost::asio;


namespace {
// 网关入口帧：注册/登录使用独立区间，避免与业务 type（101=单聊等）冲突
constexpr int32_t kWsRegisterType = 90;
constexpr int32_t kWsLoginType = 91;
} // namespace

WSSession::WSSession(asio::ssl::stream<tcp::socket> &&stream,
                     AuthClient *auth_client, MsgClient *msg_client,
                     PushClient *push_client, GroupClient *group_client,
                     FriendClient *friend_client,
                     ConversationClient *conversation_client,
                     GatewayRouter *router, std::string instance_id)
    : ws_(std::move(stream)), session_id_(uuid::newone_str()),
      auth_client_(auth_client), msg_client_(msg_client),
      push_client_(push_client), group_client_(group_client),
      friend_client_(friend_client),
      conversation_client_(conversation_client), router_(router),
      instance_id_(std::move(instance_id)) {}

WSSession::~WSSession() = default;

/*
  TCP建立之后，由start函数处理TLS和WebSocket
*/
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
                     self->ws_.next_layer().next_layer().remote_endpoint().address().to_string());

        // 3. 读取第一个应用帧（注册或登陆）
        bool keepAlive = false;
        try {
          keepAlive = co_await self->doRead();
        } catch (const std::exception &e) {
          spdlog::error("WSSession: session failed: {}", e.what());
        } catch (...) {
          spdlog::error("WSSession: session failed: unknown exception");
        }
        if (!keepAlive){
          co_await self->onDisconnect();
        } 
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
  if (!regReq.ParseFromString(regStr)) {      // 如果注册的请求无法解析
    spdlog::warn("WSSession: invalid register message (Parse error occurred), raw_size={}", regStr.size());

    // 发送错误响应（Protobuf 二进制）
    sdkws::SdkWSResp errResp;
    errResp.set_errorcode("PARSE_ERROR");
    errResp.set_errormsg("Parse error occurred");
    errResp.set_requestid(req.requestid());
    errResp.set_type(req.type()); // 对应注册账号req的类型
    std::string errBin = errResp.SerializeAsString();

    ws_.binary(true);
    co_await ws_.async_write(asio::buffer(errBin), asio::redirect_error(asio::use_awaitable, ec));
    co_await ws_.async_close(websocket::close_code::policy_error, asio::redirect_error(asio::use_awaitable, ec));
    co_return;
  }

  if (regReq.phone().empty() || regReq.nickname().empty() || regReq.password().empty()) {   // 如果请求的字段非法
    spdlog::warn("WSSession: invalid register message (register info imcomplete)");

    // 发送错误响应（Protobuf 二进制）
    sdkws::SdkWSResp errResp;
    errResp.set_errorcode("IMCOMPLETE_INFO");
    errResp.set_errormsg("Register info imcomplete");
    errResp.set_requestid(req.requestid());
    errResp.set_type(req.type());
    std::string errBin = errResp.SerializeAsString();

    ws_.binary(true);
    co_await ws_.async_write(asio::buffer(errBin), asio::redirect_error(asio::use_awaitable, ec));
    co_await ws_.async_close(websocket::close_code::policy_error, asio::redirect_error(asio::use_awaitable, ec));
    co_return;
  }

  // 注册请求通过合法检查
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
  wsResp.set_type(kWsRegisterType);
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
  ape::obs::RecordWsAuth(resp.success() ? "register_ok" : "register_error");

  std::string binary = wsResp.SerializeAsString();
  ws_.binary(true);
  co_await ws_.async_write(asio::buffer(binary), asio::redirect_error(asio::use_awaitable, ec));
  co_await ws_.async_close(websocket::close_code::normal, asio::redirect_error(asio::use_awaitable, ec));

  co_return;
}

boost::asio::awaitable<void> WSSession::handleAuth(sdkws::SdkWSReq &req) {
  auto self = shared_from_this();
  boost::system::error_code ec;

  if (req.account() == 0) {   // 账号为0视为缺少字段
    ape::obs::RecordWsAuth("missing_account");
    spdlog::warn("WSSession: invalid auth protobuf message (missing account)");

    // 发送错误响应（Protobuf 二进制）
    sdkws::SdkWSResp errResp;
    errResp.set_errorcode("INVALID_PARAM");
    errResp.set_errormsg("missing account");
    errResp.set_type(0);
    std::string errBin = errResp.SerializeAsString();

    ws_.binary(true);
    co_await ws_.async_write(asio::buffer(errBin), asio::redirect_error(asio::use_awaitable, ec));
    co_await ws_.async_close(websocket::close_code::policy_error, asio::redirect_error(asio::use_awaitable, ec));
    co_return;
  }

  const uint64_t account_ = req.account();
  const std::string plainPWD_ = req.data();
  std::string token_ = req.token();
  spdlog::info("WSSession: auth request login from user={}, reqType={}, trackID={}", account_, req.type(), req.trackid());

  bool token_valid = false;

  //分支一：token 为空，走密码登录
  if (token_.empty()) {
    auto resp = co_await auth_client_->LoginRequest(account_, plainPWD_);
    if (!resp.success()) {
      ape::obs::RecordWsAuth("login_failed");
      spdlog::warn("WSSession: LoginRequest failed for account={}, reason={}", account_, resp.errormessage());

      // 必须发错误帧再关，否则客户端只看到close(1000)什么都收不到
      sdkws::SdkWSResp errResp;
      errResp.set_errorcode("401");
      errResp.set_errormsg("login failed: " + resp.errormessage());
      errResp.set_account(account_);
      errResp.set_type(0);
      errResp.set_requestid(req.requestid());
      std::string errBin = errResp.SerializeAsString();

      ws_.binary(true);
      co_await ws_.async_write(asio::buffer(errBin), asio::redirect_error(asio::use_awaitable, ec));
      co_await ws_.async_close(websocket::close_code::policy_error, asio::redirect_error(asio::use_awaitable, ec));
      co_return;
    }

    // 登录成功，保存 token 供回传与后续复用
    token_ = resp.token();
    token_valid = true;
    spdlog::info("WSSession: LoginRequest ok for account={}, token issued", account_);
  } else {  // 分支二：token 非空，走token校验
      try {
        auto resp = co_await auth_client_->ValidateToken(token_);
        if (resp.valid() && resp.account() == account_) {
          token_valid = true;
          spdlog::info("WSSession: token verified via AuthService for userID {} (account: {})", resp.userid(), std::to_string(account_));
      } else {
        spdlog::warn("WSSession: AuthService token validation failed for account={}", account_);
      }
    } catch (const std::exception &e) {
      spdlog::error("WSSession: AuthService ValidateToken error: {}", e.what());
    }
  }

  // ── 认证失败统一处理 ──
  if (!token_valid) {
    ape::obs::RecordWsAuth("token_invalid");
    spdlog::warn("WSSession: auth failed for account {} - invalid or expired token", account_);

    sdkws::SdkWSResp errResp;
    errResp.set_errorcode("INVALID_PARAM");
    errResp.set_errormsg("invalid or expired token");
    errResp.set_account(account_);
    errResp.set_type(0);
    errResp.set_requestid(req.requestid());
    std::string errBin = errResp.SerializeAsString();

    ws_.binary(true);
    co_await ws_.async_write(asio::buffer(errBin), asio::redirect_error(asio::use_awaitable, ec));
    co_await ws_.async_close(websocket::close_code::policy_error, asio::redirect_error(asio::use_awaitable, ec));
    co_return;
  }

  // 认证成功
  ape::obs::RecordWsAuth("login_ok");
  this->account_ = account_;
  spdlog::info("WSSession: user {} authenticated on instance {}", account_,
               instance_id_);

  // 连接数上限（PR-6）：超限时不注册，回 SERVER_BUSY 让客户端退避重连
  if (WSSessionManager::instance().connectionCount() >=
          WSSessionManager::MaxConnections() &&
      !WSSessionManager::instance().hasSession(account_)) {
    sdkws::SdkWSResp busy;
    busy.set_type(0);
    busy.set_account(account_);
    busy.set_requestid(req.requestid());
    busy.set_errorcode("SERVER_BUSY");
    busy.set_errormsg("gateway connection limit reached");
    ws_.binary(true);
    co_await ws_.async_write(asio::buffer(busy.SerializeAsString()),
                             asio::redirect_error(asio::use_awaitable, ec));
    co_await ws_.async_close(websocket::close_code::policy_error,
                             asio::redirect_error(asio::use_awaitable, ec));
    co_return;
  }

  // 启动写队列（此后所有 ws_.async_write 统一走 queuedWrite）
  startWriteQueue();

  // 路由表：先处理旧持有者，再无条件覆盖（4.3）。第 3 步覆盖后旧连接再也
  // 收不到新消息，其下一次 CAS 续期会失败并自杀——互踢的兜底路径。
  if (GatewayRouter::RoutingEnabled() && router_ != nullptr) {
    const auto previous = co_await router_->Resolve(account_);
    if (previous.has_value() && *previous != instance_id_) {
      co_await router_->KickUser(account_, "duplicate_login", instance_id_);
      spdlog::info("WSSession: user {} logged in on {}, kicking old holder {}",
                   account_, instance_id_, *previous);
    }
    try {
      co_await RedisConnector::instance().setex(
          ape::gateway::RouteKey(account_), ape::gateway::kRouteTtlSeconds,
          routeValue());
      route_owned_ = true;
    } catch (const std::exception &e) {
      // 写路由失败不影响登录：路由失效只影响性能，由广播兜底（INV-3）
      spdlog::error("WSSession: write gateway route failed for user {}: {}",
                    account_, e.what());
    }
  }

  // 注册到 SessionManager（同实例重复登录由 manager 踢旧连接，C-7）
  WSSessionManager::instance().registerSession(account_, self);

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

  // 登录同步：按会话有界快照，不等待 ACK；失败只回 209，不断连接。
  std::exception_ptr syncFailure;
  try {
    co_await syncOffline(req);
  } catch (...) {
    syncFailure = std::current_exception();
  }
  if (syncFailure) {
    try {
      std::rethrow_exception(syncFailure);
    } catch (const std::exception &e) {
      spdlog::error("WSSession: syncOffline error for user {}: {}", account_, e.what());
    }
    if (!disconnected_) {
      sdkws::SdkWSResp failed;
      failed.set_type(209);
      failed.set_account(account_);
      failed.set_errorcode("SYNC_ERROR");
      failed.set_errormsg("Offline message synchronization failed; retry later");
      co_await queuedWrite(std::make_shared<std::string>(failed.SerializeAsString()));
    }
  }

  // 群同步：单聊 209 之后发 251 内联（小群）并以 210 收尾；失败也发 210。
  try {
    co_await syncGroups(req);
  } catch (const std::exception &e) {
    spdlog::error("WSSession: syncGroups error for user {}: {}", account_, e.what());
  }

  co_return;
}

boost::asio::awaitable<void> WSSession::fallback(sdkws::SdkWSReq &req) {
  auto self = shared_from_this();
  boost::system::error_code ec;
  
  ape::obs::RecordWsFrame(static_cast<int32_t>(req.type()), "unsupported");
  spdlog::warn("WSSession: message type unsupported!");
  sdkws::SdkWSResp errResp;

  errResp.set_errorcode("4401");
  errResp.set_errormsg("message type unsupported");
  errResp.set_type(0);
  errResp.set_requestid(req.requestid());
  std::string errBin = errResp.SerializeAsString();
  
  ws_.binary(true);
  co_await ws_.async_write(asio::buffer(errBin), asio::redirect_error(asio::use_awaitable, ec));
  co_await ws_.async_close(websocket::close_code(4404), asio::redirect_error(asio::use_awaitable, ec));
  
  co_return;
}

asio::awaitable<bool> WSSession::doRead() {
  auto self = shared_from_this();
  boost::system::error_code ec;

  // 设置合理超时
  ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));

  co_await ws_.async_read(buffer_, asio::redirect_error(asio::use_awaitable, ec));
  if (ec) {
    spdlog::error("WSSession: Read first Frame failed: {}", ec.message());
    co_return false;
  }

  // 解析SdkWSReq帧
  std::string reqData = beast::buffers_to_string(buffer_.data());
  buffer_.consume(buffer_.size());
  sdkws::SdkWSReq sdkReq;

  if (!sdkReq.ParseFromString(reqData)) {
    spdlog::warn("WSSession: invalid protobuf message (Parse error occurred), raw_size={}", reqData.size());

    // 发送错误响应并关闭连接
    sdkws::SdkWSResp errResp;
    errResp.set_errorcode("1");
    errResp.set_errormsg("Parse error occurred");
    errResp.set_type(0);
    std::string errBin = errResp.SerializeAsString();

    ws_.binary(true);
    co_await ws_.async_write(asio::buffer(errBin), asio::redirect_error(asio::use_awaitable, ec));
    co_await ws_.async_close(websocket::close_code::policy_error, asio::redirect_error(asio::use_awaitable, ec));
    co_return false;
  }

  // 分别处理注册或登陆
  switch (sdkReq.type()) {
  case kWsRegisterType:
    co_await handleReg(sdkReq);
    co_return false;    // 返回false让上层关闭连接，因为注册已经结束，注册结束处同样有关闭逻辑，两处地方是幂等的
  case kWsLoginType:
    co_await handleAuth(sdkReq);
    co_return account_ != 0;    // 认证成功session的account_字段非0
  default:
    co_await fallback(sdkReq);
    co_return false;
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
        spdlog::error(
            "WSSession: read error for user {}: {}, category={}, code={}",
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
      // 心跳先续期路由（CAS），续期失败说明路由已被其他实例持有，本连接必须
      // 自杀，否则会在 30s 窗口里继续"活着"。续期成功后才回 pong，
      // 避免把已失去所有权的连接确认成健康。
      // 优雅下线期间停止续期，避免占用路由（4.8 第 4 步）。
      if (!draining_ && !WSSessionManager::instance().draining()) {
        if (!co_await renewRoute()) {
          kick("route moved to another instance");
          // 让写队列有机会把 KICKED 帧写出去；kick 内部 200ms 后会断开。
          // 不能立刻 break，否则 doReadLoop 退出时的 onDisconnect 会抢在
          // 帧落盘前关闭 socket。
          asio::steady_timer kick_timer(ws_.get_executor());
          kick_timer.expires_after(std::chrono::milliseconds(250));
          boost::system::error_code ignored;
          co_await kick_timer.async_wait(
              asio::redirect_error(asio::use_awaitable, ignored));
          break;
        }
      }

      sdkws::SdkWSResp pongResp;
      pongResp.set_requestid(req.requestid());
      pongResp.set_errorcode("0");
      pongResp.set_errormsg("pong");
      pongResp.set_account(account_);
      pongResp.set_type(0);
      std::string pongBin = pongResp.SerializeAsString();

      ws_.binary(true);
      co_await queuedWrite(std::make_shared<std::string>(std::move(pongBin)));
      continue;
    }

    // ── 可观测：每个业务帧一个 root span；RAII 在 continue/异常时结束 ──
    ape::otel::SpanHandle frame_span = ape::otel::StartRootSpan(
        "WS " + std::to_string(reqType),
        {{"ws.type", std::to_string(reqType)},
         {"ws.account", std::to_string(account_)},
         {"ws.request_id", req.requestid()}});
    const auto frame_trace = frame_span.context();
    const auto frame_started = std::chrono::steady_clock::now();
    struct FrameObserver {
      ape::otel::SpanHandle *span;
      int32_t type;
      std::chrono::steady_clock::time_point started;
      const char *result;
      ~FrameObserver() {
        const auto ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - started).count();
        ape::obs::RecordWsFrame(type, result);
        ape::obs::RecordWsRequestDuration(type, ms);
        if (span != nullptr && span->valid()) {
          if (std::string_view(result) == "ok")
            span->EndOk();
          else
            span->EndError(result);
        }
      }
    } frame_observer{&frame_span, reqType, frame_started, "ok"};

    // type=101: 单聊消息
    if (reqType == 101) {
      sdkws::SendMessageReq sendMsgReq;
      std::string respBin;
      bool hasResp = false;

      if (sendMsgReq.ParseFromString(req.data())) {
        for (auto &message : *sendMsgReq.mutable_msgs())
          message.set_sendid(account_);

        const auto send_started = std::chrono::steady_clock::now();
        bool send_ok = true;
        try {
          auto sendResp = co_await msg_client_->SendMessages(sendMsgReq, frame_trace);

          sdkws::SdkWSResp wsResp;
          wsResp.set_requestid(req.requestid());
          wsResp.set_errorcode("0");
          wsResp.set_errormsg("send message success");
          wsResp.set_account(account_);
          wsResp.set_type(101);
          wsResp.set_logid(frame_trace.traceId());
          wsResp.set_data(sendResp.SerializeAsString());
          respBin = wsResp.SerializeAsString();
          hasResp = true;
        } catch (const std::exception &e) {
          send_ok = false;
          frame_observer.result = "error";
          APE_LOG_ERROR("ws.send.failed").Field("account", account_).Field("error", e.what()).Trace(frame_trace);
          spdlog::error("WSSession: SendMessages failed for user {}: {}",
                        account_, e.what());

          sdkws::SdkWSResp errResp;
          errResp.set_requestid(req.requestid());
          errResp.set_errorcode("500");
          errResp.set_errormsg(std::string("SendMessages failed: ") + e.what());
          errResp.set_account(account_);
          errResp.set_type(101);
          errResp.set_logid(frame_trace.traceId());
          respBin = errResp.SerializeAsString();
          hasResp = true;
        }

        const auto send_ms = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - send_started).count();
        ape::obs::RecordSend("single", send_ok ? "ok" : "error", send_ms);
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
      co_await handleAck(req, frame_trace);
      continue;
    }

    // 群消息发送（246）：单副本 Mongo，无 ACK
    if (reqType == 246) {
      groupchat::SendGroupMessageReq command;
      groupchat::SendGroupMessageResp reply;
      if (!command.ParseFromString(req.data())) {
        reply.set_errorcode("INVALID_PARAM");
        reply.set_errormsg("Invalid group send request");
      } else if (!group_client_) {
        reply.set_errorcode("SERVICE_UNAVAILABLE");
        reply.set_errormsg("Group service unavailable");
      } else {
        command.set_actor(account_);
        const auto group_started = std::chrono::steady_clock::now();
        reply = co_await group_client_->SendGroupMessages(command, frame_trace);
        const auto group_ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - group_started).count();
        ape::obs::RecordSend("group", reply.errorcode() == "0" ? "ok" : "error", group_ms);
        if (reply.errorcode() != "0")
          frame_observer.result = "error";
      }
      sdkws::SdkWSResp response;
      response.set_type(246);
      response.set_account(account_);
      response.set_requestid(req.requestid());
      response.set_logid(frame_trace.traceId());
      response.set_errorcode(reply.errorcode());
      response.set_errormsg(reply.errormsg());
      response.set_data(reply.SerializeAsString());
      co_await queuedWrite(
          std::make_shared<std::string>(response.SerializeAsString()));
      continue;
    }

    // 群历史（245）：无锁读 Mongo，错误码在外层 SdkWSResp
    if (reqType == 245) {
      groupchat::GetGroupChatHistoryReq command;
      sdkws::SdkWSResp response;
      response.set_type(245);
      response.set_account(account_);
      response.set_requestid(req.requestid());
      response.set_logid(frame_trace.traceId());
      if (!command.ParseFromString(req.data()) || command.groupid().empty()) {
        response.set_errorcode("INVALID_PARAM");
        response.set_errormsg("Invalid group history request");
      } else if (!group_client_) {
        response.set_errorcode("SERVICE_UNAVAILABLE");
        response.set_errormsg("Group service unavailable");
      } else {
        command.set_owneruserid(account_);
        const auto result = co_await group_client_->GetGroupChatHistory(command, frame_trace);
        response.set_errorcode(result.errorCode);
        response.set_errormsg(result.errorMessage);
        if (result.errorCode == "0")
          response.set_data(result.history.SerializeAsString());
      }
      co_await queuedWrite(
          std::make_shared<std::string>(response.SerializeAsString()));
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
        const int operation = static_cast<int>(command.operation());
        if (operation >= 11 && operation <= 14) {
          // 旧 250 Send(11)/Pull(12)/Ack(13)/Read(14)：v3 直接切新协议。
          reply.set_errorcode("UNSUPPORTED_OPERATION");
          reply.set_errormessage("旧群消息操作已废弃，请使用 246/245");
          reply.set_groupid(command.groupid());
        } else if (group_client_) {
          reply = co_await group_client_->Execute(command);
        } else {
          reply.set_errorcode("DB_ERROR");
          reply.set_errormessage("Group service unavailable");
        }
      }

      sdkws::SdkWSResp response;
      response.set_type(250);
      response.set_account(account_);
      response.set_requestid(req.requestid());
      response.set_data(reply.SerializeAsString());
      ws_.binary(true);
      co_await queuedWrite(
          std::make_shared<std::string>(response.SerializeAsString()));
      continue;
    }

    // 拉取历史消息
    if (reqType == 244) {
      sdkws::SdkWSResp response;
      response.set_type(244);
      response.set_requestid(req.requestid());
      response.set_account(account_);
      response.set_logid(frame_trace.traceId());
      response.set_errorcode("0");
      msg::GetSingleChatHistoryReq command;
      if (!command.ParseFromString(req.data()) || !command.peeruserid()) {
        response.set_errorcode("INVALID_PARAM");
        response.set_errormsg("Invalid history request");
      } else {
        command.set_owneruserid(account_);
        try {
          auto result = co_await msg_client_->GetSingleChatHistory(command, frame_trace);
          response.set_data(result.SerializeAsString());
        } catch (const std::exception &e) {
          response.set_errorcode("HISTORY_ERROR");
          response.set_errormsg(e.what());
        }
      }
      co_await queuedWrite(
          std::make_shared<std::string>(response.SerializeAsString()));
      continue;
    }

    // 会话相关
    if (reqType >= 240 && reqType <= 243) {
      sdkws::SdkWSResp response;
      response.set_type(reqType);
      response.set_requestid(req.requestid());
      response.set_account(account_);
      auto apply_result = [&response](const conversation::OperationResult &info) {
        response.set_errorcode(info.errorcode().empty() ? "0" : info.errorcode());
        response.set_errormsg(info.errormessage());
      };
      try {
        if (!conversation_client_)
          throw std::runtime_error("Conversation service unavailable");
        if (reqType == 240) { // 获取全部会话
          auto result = co_await conversation_client_->List(account_);
          response.set_data(result.SerializeAsString());
          apply_result(result.result());
        } else if (reqType == 241) {
          conversation::MarkConversationReadReq command;
          if (!command.ParseFromString(req.data()) ||
              command.conversationid().empty())
            throw std::invalid_argument("Invalid read request");
          command.set_owneruserid(account_);
          auto result = co_await conversation_client_->MarkRead(command);
          response.set_data(result.SerializeAsString());
          apply_result(result.result());
        } else if (reqType == 242) {
          conversation::UpdateConversationSettingsReq command;
          if (!command.ParseFromString(req.data()) ||
              command.conversationid().empty())
            throw std::invalid_argument("Invalid settings request");
          command.set_owneruserid(account_);
          auto result = co_await conversation_client_->UpdateSettings(command);
          response.set_data(result.SerializeAsString());
          apply_result(result.result());
        } else {
          conversation::DeleteConversationReq command;
          if (!command.ParseFromString(req.data()) ||
              command.conversationid().empty())
            throw std::invalid_argument("Invalid delete request");
          command.set_owneruserid(account_);
          auto result = co_await conversation_client_->Delete(command);
          response.set_data(result.SerializeAsString());
          apply_result(result.result());
        }
      } catch (const std::exception &e) {
        response.set_errorcode("CONVERSATION_ERROR");
        response.set_errormsg(e.what());
      }
      co_await queuedWrite(
          std::make_shared<std::string>(response.SerializeAsString()));
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
        } catch (const std::exception &e) {
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
      co_await queuedWrite(
          std::make_shared<std::string>(response.SerializeAsString()));
      if (reply.errorcode() == "0") {
        for (const auto account : reply.changedaccounts()) {
          if (!WSSessionManager::instance().hasSession(account)) {
            spdlog::debug("Friend notification deferred to login sync, user={}",
                          account);
            continue;
          }
          sdkws::SdkWSResp notification;
          notification.set_type(230);
          notification.set_account(account);
          notification.set_errorcode("0");
          WSSessionManager::instance().pushToUser(
              account,
              std::make_shared<std::string>(notification.SerializeAsString()));
        }
      }
      continue;
    }

    // 其他业务消息类型暂不做处理，仅记录日志
    frame_observer.result = "unsupported";
    APE_LOG_WARN("ws.type.unsupported")
        .Field("account", account_)
        .Field("type", reqType)
        .Field("request_id", req.requestid());
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
    if (self->disconnected_)
      return;
    if (!self->write_queue_ ||
        !self->write_queue_->try_send(boost::system::error_code{}, payload)) {
      ape::obs::RecordWsWriteDrop("push_queue_full");
      // 异步推送无法向调用方返回队列失败，关闭连接让未 ACK 消息重登恢复。
      asio::co_spawn(
          self->ws_.get_executor(),
          [self]() -> asio::awaitable<void> { co_await self->onDisconnect(); },
          asio::detached);
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
    if (write_queue_depth_ > 0)
      --write_queue_depth_;
    ape::obs::AddWsWriteQueueFrames(-1);
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
    ape::obs::RecordWsWriteDrop("write_queue_full");
    // 入队失败不是发送成功；停止本连接，供客户端重连后按 cursor 续传（10.6）
    co_await onDisconnect();
    throw boost::system::system_error(asio::error::operation_aborted);
  }
  ++write_queue_depth_;
  ape::obs::AddWsWriteQueueFrames(1);
  co_return;
}

boost::asio::awaitable<void> WSSession::handleAck(const sdkws::SdkWSReq &req,
                                                   const ape::otel::TraceContext &trace) {
  sdkws::AckReq ackReq;
  if (!ackReq.ParseFromString(req.data()) ||
      (ackReq.acktype() != 1 && ackReq.acktype() != 2) ||
      ackReq.conversations_size() == 0) {
    co_return;
  }

  // 网关过滤（5.3）：空 conversationID / upToSeq<=0 直接丢弃，过滤后为空则忽略整帧
  std::vector<const sdkws::ConversationAck *> accepted;
  accepted.reserve(ackReq.conversations_size());
  for (const auto &ack : ackReq.conversations()) {
    if (ack.conversationid().empty() || ack.uptoseq() <= 0)
      continue;
    accepted.push_back(&ack);
  }
  if (accepted.empty())
    co_return;

  ape::obs::RecordAck(ackReq.acktype() == 1 ? "online" : "offline",
                     static_cast<int64_t>(accepted.size()));

  // 单帧会话数上限（13.4）：超过上限时按 100 个一批拆成多个 RPC，
  // 语义仍是“整帧一次确认”，不改变每个会话的处理结果。
  constexpr std::size_t kMaxAcksPerRequest = 100;
  bool ackFailed = false;
  for (std::size_t offset = 0; offset < accepted.size() && !ackFailed;
       offset += kMaxAcksPerRequest) {
    push::AckConversationsReq request;
    request.set_userid(account_);
    request.set_acktype(ackReq.acktype());
    const auto end = std::min(accepted.size(), offset + kMaxAcksPerRequest);
    for (std::size_t i = offset; i < end; ++i) {
      auto *item = request.add_conversations();
      item->set_conversationid(accepted[i]->conversationid());
      item->set_uptoseq(accepted[i]->uptoseq());
    }
    try {
      co_await push_client_->AckConversations(request, trace);
    } catch (const std::exception &e) {
      spdlog::error("WSSession: ACK failed for user {}: {}", account_, e.what());
      ackFailed = true;
    }
  }
  // PushService 不可用/协议错误：断连由客户端重连后重发，服务端已提交的部分幂等（10）
  if (ackFailed)
    co_await onDisconnect();
}

boost::asio::awaitable<void> WSSession::onDisconnect() {

  if (disconnected_){
    co_return;
  }
  
  disconnected_ = true;

  if (write_queue_) {
    write_queue_->cancel();
    write_queue_->close();
  }

  // 关闭最底层socket连接
  boost::system::error_code closeError;
  auto &socket = beast::get_lowest_layer(ws_);
  socket.cancel(closeError);
  socket.shutdown(tcp::socket::shutdown_both, closeError);
  socket.close(closeError);

  // 清理session
  if (account_ == 0){
    co_return;
  }
  bool isActive = WSSessionManager::instance().unregisterSession(account_, shared_from_this());
  if (!isActive){
    co_return;
  }

  // 路由表不在这里删除：删除也可能失败，TTL(300s) 是唯一可靠的清理方式（4.2）。
  // 多留的旧值由广播兜底吸收（INV-2/INV-3）。
  route_owned_ = false;
}

// ── 主动踢下线 / 网关心跳下线 ──

void WSSession::kick(const std::string &reason) {
  ape::obs::RecordWsKick(reason);
  APE_LOG_INFO("ws.kick").Field("account", account_).Field("reason", reason);
  auto self = shared_from_this();
  asio::post(ws_.get_executor(), [self, reason]() {
    if (self->disconnected_)
      return;
    sdkws::SdkWSResp frame;
    frame.set_type(0);
    frame.set_account(self->account_);
    frame.set_errorcode("KICKED");
    frame.set_errormsg(reason.empty() ? "account logged in elsewhere" : reason);
    self->asyncSend(std::make_shared<std::string>(frame.SerializeAsString()));

    // 尽力把 KICKED 帧写出去，再断开；继续持有 shared_ptr 直到协程结束
    auto timer = std::make_shared<asio::steady_timer>(self->ws_.get_executor());
    timer->expires_after(std::chrono::milliseconds(200));
    asio::co_spawn(
        self->ws_.get_executor(),
        [self, timer]() -> asio::awaitable<void> {
          boost::system::error_code ignored;
          co_await timer->async_wait(
              asio::redirect_error(asio::use_awaitable, ignored));
          co_await self->onDisconnect();
        },
        asio::detached);
  });
}

void WSSession::notifyDraining() {
  auto self = shared_from_this();
  asio::post(ws_.get_executor(), [self]() {
    if (self->disconnected_)
      return;
    sdkws::SdkWSResp frame;
    frame.set_type(0);
    frame.set_account(self->account_);
    frame.set_errorcode("SERVER_DRAINING");
    frame.set_errormsg("gateway is draining, reconnect to another instance");
    // sending 字段：仅在已建立写队列时入队
    self->asyncSend(std::make_shared<std::string>(frame.SerializeAsString()));
  });
}

void WSSession::closeNow() {
  auto self = shared_from_this();
  asio::post(ws_.get_executor(), [self]() {
    asio::co_spawn(
        self->ws_.get_executor(),
        [self]() -> asio::awaitable<void> { co_await self->onDisconnect(); },
        asio::detached);
  });
}

boost::asio::awaitable<bool> WSSession::renewRoute() {
  if (!route_owned_ || !GatewayRouter::RoutingEnabled())
    co_return true;
  try {
    const auto renewed = co_await RedisConnector::instance().eval(
        ape::gateway::kRouteRenewScript, {ape::gateway::RouteKey(account_)},
        {routeValue(), std::to_string(ape::gateway::kRouteTtlSeconds)});
    if (renewed == 0) {
      ape::obs::RecordWsRouteRenewFailure();
      spdlog::warn("WSSession: route ownership lost for user={}, disconnecting",
                   account_);
      co_return false;
    }
    co_return true;
  } catch (const std::exception &e) {
    // Redis 抖动不能放大成集体掉线：本次续期跳过，下一心跳再试
    spdlog::error("WSSession: route renew error for user={}: {}", account_,
                  e.what());
    co_return true;
  }
}

boost::asio::awaitable<void>
WSSession::waitWriteQueueBelow(std::size_t threshold,
                               std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!disconnected_ && write_queue_depth_ >= threshold &&
         std::chrono::steady_clock::now() < deadline) {
    asio::steady_timer timer(ws_.get_executor());
    timer.expires_after(std::chrono::milliseconds(10));
    boost::system::error_code ec;
    co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
  }
}

// ── PR-8：按会话同步（有界快照，不再等 ACK） ──
boost::asio::awaitable<void>
WSSession::syncOffline(const sdkws::SdkWSReq &req) {
  constexpr uint32_t kMaxConversations = 50;
  constexpr int kPerConversation = 20;
  constexpr int kMaxFramesPerRound = 50;

  // 客户端上报的每会话水位；没有就从头推最近 20 条（等价于一次对账）
  std::map<std::string, int64_t> cursors;
  for (const auto &cursor : req.cursors()) {
    if (cursor.conversationid().empty())
      continue;
    cursors[cursor.conversationid()] = std::max<int64_t>(0, cursor.lastseq());
  }

  std::vector<conversation::Conversation> conversations;
  std::exception_ptr listFailure;
  try {
    conversations = co_await MySQLHandler::ListRecentConversations(
        account_, kMaxConversations);
  } catch (...) {
    listFailure = std::current_exception();
  }
  if (listFailure) {
    try {
      std::rethrow_exception(listFailure);
    } catch (const std::exception &e) {
      spdlog::error("WSSession: list conversations failed for user {}: {}",
                    account_, e.what());
    }
    sdkws::SdkWSResp failed;
    failed.set_type(209);
    failed.set_account(account_);
    failed.set_errorcode("SYNC_ERROR");
    failed.set_errormsg("Conversation list unavailable; retry later");
    co_await queuedWrite(
        std::make_shared<std::string>(failed.SerializeAsString()));
    co_return;
  }

  sdkws::SyncSummary summary;
  summary.set_conversationstotal(
      static_cast<uint32_t>(conversations.size()));
  summary.set_hasmore(conversations.size() >= kMaxConversations);
  bool partial = false;

  for (const auto &conversation : conversations) {
    if (disconnected_)
      co_return;
    const auto conv_id = conversation.conversationid();
    if (conv_id.empty()) {
      partial = true;
      continue;
    }
    const auto cursor_it = cursors.find(conv_id);
    const int64_t from = cursor_it == cursors.end() ? 0 : cursor_it->second;

    std::vector<sdkws::MsgData> messages;
    try {
      messages = co_await MongoHandler::LatestAfterMessagesAsync(
          account_, conv_id, from, kPerConversation);
    } catch (const std::exception &e) {
      spdlog::warn("WSSession: sync conversation {} failed for user {}: {}",
                   conv_id, account_, e.what());
      partial = true;
      continue;
    }
    if (messages.empty())
      continue;

    // 脏数据：跳过该条并标记 PARTIAL，不能让整轮中止（O-2）
    std::vector<sdkws::MsgData> valid;
    valid.reserve(messages.size());
    for (auto &message : messages) {
      if (message.servermsgid().empty() ||
          !message_validation::IsSingleMessageFor(account_, message)) {
        partial = true;
        spdlog::warn("WSSession: skip invalid offline message conv={} user={}",
                     conv_id, account_);
        continue;
      }
      valid.push_back(std::move(message));
    }
    if (valid.empty())
      continue;

    // 按会话分轮下发，每轮 ≤50 帧（本模型下 ≤20，天然满足 Q-10）
    for (std::size_t offset = 0; offset < valid.size();
         offset += kMaxFramesPerRound) {
      if (disconnected_)
        co_return;
      const std::size_t end =
          std::min(valid.size(), offset + kMaxFramesPerRound);
      co_await waitWriteQueueBelow(128, std::chrono::seconds(1));
      for (std::size_t i = offset; i < end; ++i) {
        const auto &message = valid[i];
        sdkws::SdkWSResp frame;
        frame.set_type(200);
        frame.set_account(account_);
        frame.set_errorcode("0");
        frame.set_errormsg("offline message");
        frame.set_requestid(message.servermsgid());
        frame.set_data(message.SerializeAsString());
        // 入队成功即完成（不等 ACK）；客户端按 serverMsgID 去重
        co_await queuedWrite(
            std::make_shared<std::string>(frame.SerializeAsString()));
        summary.set_messagespushed(summary.messagespushed() + 1);
      }
    }

    summary.set_conversationssynced(summary.conversationssynced() + 1);
    auto *range = summary.add_ranges();
    range->set_conversationid(conv_id);
    range->set_fromseq(valid.front().seq());
    range->set_toseq(valid.back().seq());
    range->set_hasgap(valid.front().seq() > from + 1);
  }

  if (disconnected_)
    co_return;

  sdkws::SdkWSResp done;
  done.set_type(209);
  done.set_account(account_);
  done.set_errorcode(partial ? "PARTIAL" : "0");
  done.set_errormsg(partial ? "partial offline sync" : "offline sync complete");
  done.set_data(summary.SerializeAsString());
  co_await queuedWrite(
      std::make_shared<std::string>(done.SerializeAsString()));
  spdlog::info(
      "WSSession: offline sync complete, user={}, conversations={}, pushed={}",
      account_, summary.conversationssynced(), summary.messagespushed());
}

// ── 群同步（210）：251 内联 + 结束信号 ──
namespace {

int64_t GroupSyncEnv(const char *name, int64_t fallback, int64_t minimum, int64_t maximum) {
  const char *value = std::getenv(name);
  if (!value || !*value)
    return fallback;
  try {
    return std::clamp<int64_t>(std::stoll(value), minimum, maximum);
  } catch (const std::exception &) {
    spdlog::warn("WSSession: invalid {}={}, using {}", name, value, fallback);
    return fallback;
  }
}

} // namespace

boost::asio::awaitable<void> WSSession::syncGroups(const sdkws::SdkWSReq &req) {
  const auto maxGroups = static_cast<uint32_t>(
      GroupSyncEnv("GROUP_SYNC_MAX_GROUPS", 50, 1, 5000));
  const auto perGroup = GroupSyncEnv("GROUP_SYNC_MESSAGES_PER_GROUP", 20, 1, 1000);
  const auto maxMessages = GroupSyncEnv("GROUP_SYNC_MAX_MESSAGES", 200, 1, 100000);
  const auto inlineBytes = GroupSyncEnv("GROUP_INLINE_MAX_BYTES", 262144, 1024, 16 * 1024 * 1024);
  const auto inlineMembers = GroupSyncEnv("GROUP_INLINE_MAX_MEMBERS", 200, 1, 1000000);

  // 91 的 cursors 同时承载单聊会话（min_max）与群（groupID）；这里只认群。
  std::map<std::string, int64_t> cursors;
  for (const auto &cursor : req.cursors()) {
    if (cursor.conversationid().empty())
      continue;
    cursors[cursor.conversationid()] = std::max<int64_t>(0, cursor.lastseq());
  }

  groupchat::GroupSyncSummary summary;
  bool partial = false;
  bool hasMore = false;
  bool listFailed = false;
  std::vector<MySQLHandler::GroupRecord> groups;
  try {
    groups = co_await MySQLHandler::ListGroupsForSync(account_, maxGroups + 1);
  } catch (const std::exception &e) {
    listFailed = true;
    spdlog::error("WSSession: list groups for sync failed for {}: {}", account_, e.what());
  }
  if (listFailed) {
    if (!disconnected_) {
      sdkws::SdkWSResp failed;
      failed.set_type(210);
      failed.set_account(account_);
      failed.set_errorcode("SYNC_ERROR");
      failed.set_errormsg("Group synchronization failed; retry later");
      failed.set_data(summary.SerializeAsString());
      co_await queuedWrite(std::make_shared<std::string>(failed.SerializeAsString()));
    }
    co_return;
  }
  if (groups.size() > maxGroups) {
    hasMore = true;
    groups.resize(maxGroups);
  }

  for (const auto &group : groups) {
    if (disconnected_)
      co_return;
    const auto groupID = std::to_string(group.id);
    const int64_t joinSeq = static_cast<int64_t>(group.joinSeq);
    const int64_t lastSeq = static_cast<int64_t>(group.publishedSeq);
    const auto cursorIt = cursors.find(groupID);
    const int64_t cursor = cursorIt == cursors.end() ? 0 : cursorIt->second;
    // 客户端 cursor 已到 published_seq：该群无需同步。
    if (cursor >= lastSeq)
      continue;

    const int64_t floor = std::max<int64_t>(cursor, joinSeq);
    auto *range = summary.add_groups();
    range->set_groupid(groupID);
    range->set_fromseq(floor + 1);
    range->set_lastseq(lastSeq);
    range->set_joinseq(joinSeq);
    range->set_epoch(group.epoch);
    range->set_hasgap(floor + 1 > cursor + 1);
    // 没有内联时 toSeq=fromSeq-1（空覆盖），不是 lastSeq。
    range->set_toseq(floor);
    range->set_needspull(true);

    const int64_t pending = lastSeq - floor;
    if (static_cast<int64_t>(group.memberCount) > inlineMembers || pending > perGroup)
      continue;
    const int64_t remainingBudget =
        maxMessages - static_cast<int64_t>(summary.messagespushed());
    if (remainingBudget <= 0)
      continue;

    std::vector<sdkws::MsgData> messages;
    try {
      messages = co_await MongoHandler::GetGroupChatHistoryAsync(
          groupID, floor, lastSeq,
          static_cast<int>(std::min<int64_t>(pending, std::min<int64_t>(perGroup, remainingBudget))));
    } catch (const std::exception &e) {
      partial = true;
      spdlog::warn("WSSession: group {} sync query failed for {}: {}", groupID, account_, e.what());
      continue;
    }
    if (messages.empty())
      continue;

    groupchat::Notice notice;
    notice.set_groupid(groupID);
    notice.set_lastseq(lastSeq);
    notice.set_epoch(group.epoch);
    notice.set_refresh(false);
    notice.set_reason(0);
    notice.set_lastmsgtime(group.lastMsgTime);
    int64_t bytes = 0;
    int32_t inlined = 0;
    for (const auto &message : messages) {
      const auto size = static_cast<int64_t>(message.ByteSizeLong());
      if (bytes + size > inlineBytes)
        break;                            // 按字节截断；0 条能放下则转 needsPull
      bytes += size;
      ++inlined;
      *notice.add_messages() = message;
    }
    if (inlined == 0)
      continue;

    sdkws::SdkWSResp frame;
    frame.set_type(251);
    frame.set_account(account_);
    frame.set_errorcode("0");
    frame.set_errormsg("group sync inline");
    frame.set_data(notice.SerializeAsString());
    co_await queuedWrite(std::make_shared<std::string>(frame.SerializeAsString()));
    summary.set_messagespushed(summary.messagespushed() + inlined);
    range->set_toseq(notice.messages(inlined - 1).seq());
    // needsPull=(toSeq<lastSeq)：内联没覆盖 published_seq 就必须 Pull。
    range->set_needspull(range->toseq() < lastSeq);
  }

  if (disconnected_)
    co_return;
  summary.set_hasmore(hasMore);
  sdkws::SdkWSResp done;
  done.set_type(210);
  done.set_account(account_);
  done.set_errorcode(partial ? "PARTIAL" : "0");
  done.set_errormsg(partial ? "partial group sync" : "group sync complete");
  done.set_data(summary.SerializeAsString());
  co_await queuedWrite(std::make_shared<std::string>(done.SerializeAsString()));
  spdlog::info("WSSession: group sync complete, user={}, groups={}, pushed={}, partial={}",
               account_, summary.groups_size(), summary.messagespushed(), partial);
}
