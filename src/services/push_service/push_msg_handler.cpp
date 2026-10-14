#include "services/push_service/push_msg_handler.h"
#include "sdkws.pb.h"
#include "util/grpc_async_util.h"
#include "storage/mongohandler.h"
#include "om/otel_trace_propagation.h"
#include "om/otel_tracer.h"
#include "storage/redisconnector.h"
#include "storage/redishandler.h"
#include "storage/mysqlhandler.h"
#include <chrono>
#include <format>
#include <opentelemetry/common/attribute_value.h>
#include <opentelemetry/trace/scope.h>
#include <spdlog/spdlog.h>
#include <stdexcept>

PushHandler::PushHandler(std::shared_ptr<grpc::Channel> pushChannel)
    : pushStub_(push::PushService::NewStub(pushChannel)) {}

boost::asio::awaitable<void> PushHandler::handle(const std::string topic,
                                                 const std::string msgID) {
  // 解析 Kafka 消息: "traceparent|servermsgid" 或 仅 "servermsgid"
  std::string traceparent, serverMsgID;
  auto sep = msgID.find('|');
  if (sep != std::string::npos) {
    traceparent = msgID.substr(0, sep);
    serverMsgID = msgID.substr(sep + 1);
  } else {
    serverMsgID = msgID;
  }

  spdlog::info("PushHandler: received msgid from Kafka, topic={}, "
               "serverMsgID={}, has_tp={}",
               topic, serverMsgID, !traceparent.empty());


  // 获取完整消息（先缓存再数据库）
  std::optional<sdkws::MsgData> msgDataOpt;
  msgDataOpt = co_await RedisHandler::GetMsg(serverMsgID);
  if (!msgDataOpt.has_value()) {
    msgDataOpt = co_await MongoHandler::GetMsgByServerMsgIDAsync(serverMsgID);
  }

  if (!msgDataOpt.has_value()) {
    spdlog::info("PushHandler: message no longer exists, skipping {}", serverMsgID);
    co_return;
  }

  sdkws::MsgData msgData = msgDataOpt.value();
  // New groups have their own delivery loop; never route retired group events here.
  if (msgData.isgroupmsg()) co_return;
  if (co_await MySQLHandler::MessageDispatchDone(msgData.recvid(),serverMsgID)) co_return;

  // ── OTel: 从 traceparent 恢复父上下文，创建 Kafka Consumer Span ──
  auto tracer = ape::otel::GetTracer();
  opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> kafkaSpan;
  std::unique_ptr<opentelemetry::trace::Scope> kafkaScope;

  if (tracer && !traceparent.empty()) {
    std::vector<std::pair<std::string, std::string>> kafkaAttrs = {
        {"messaging.system", "kafka"},
        {"messaging.destination", topic},
        {"messaging.message_id", serverMsgID}};
    kafkaSpan = ape::otel::StartSpanFromEncodedTraceParent(
        traceparent, "Kafka Consume " + topic, kafkaAttrs, tracer);
    if (kafkaSpan) {
      kafkaScope = std::make_unique<opentelemetry::trace::Scope>(kafkaSpan);
    }
  }

  // 2. 构造PushMsgReq并调用PushService
  push::PushMsgReq req;
  *req.mutable_msgdata() = msgData;
  req.set_conversationid(msgData.convid());

  push::PushMsgResp resp;
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(8));
  ape::otel::InjectTraceContextToGrpcMetadata(ctx);

  auto status = co_await ape::grpc_util::GrpcAwait([&](auto &&handler) {
    pushStub_->async()->PushMsg(&ctx, &req, &resp,
                                std::forward<decltype(handler)>(handler));
  });

  if (!status.ok()) {
    // 处理失败时不确认分发完成，允许 Kafka 或 Outbox 重试
    throw std::runtime_error(
        std::format("PushHandler: gRPC PushMsg failed: {}, serverMsgID={}",
                  status.error_message(), serverMsgID));
  } else {
    spdlog::info("PushHandler: PushMsg success, serverMsgID={}", serverMsgID);
    co_await MySQLHandler::ConfirmMessageDispatch(serverMsgID);
  }

  if (kafkaSpan) {
    kafkaScope.reset(); // 恢复之前的 context
    kafkaSpan->End();
  }

  co_return;
}
