#include "services/push_service/push_msg_handler.h"
#include "sdkws.pb.h"
#include "util/grpc_async_util.h"
#include "util/mongohandler.h"
#include "util/otel_trace_propagation.h"
#include "util/otel_tracer.h"
#include "util/redisconnector.h"
#include "util/redishandler.h"
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

  // 幂等：Redis SET NX 快速拦截 + MongoDB 状态兜底
  auto markResult = co_await RedisHandler::TryMarkMsgConsumed(serverMsgID);
  if (markResult == RedisHandler::ConsumeMarkResult::Duplicate) {
    spdlog::warn("PushHandler: duplicate message skipped, serverMsgID={}",
                 serverMsgID);
    co_return;
  }
  // 如果redis挂了
  if (markResult == RedisHandler::ConsumeMarkResult::Unavailable) {
    bool alreadyConsumed =
        co_await MongoHandler::IsMsgAlreadyConsumedAsync(serverMsgID);
    if (alreadyConsumed) {
      spdlog::warn("PushHandler: msg already consumed (MongoDB fallback), "
                   "serverMsgID={}",
                   serverMsgID);
      co_return;
    }
    spdlog::warn("PushHandler: Redis unavailable, msg not yet consumed, "
                 "proceeding, serverMsgID={}",
                 serverMsgID);
  }

  // 获取完整消息（先缓存再数据库）
  std::optional<sdkws::MsgData> msgDataOpt;
  msgDataOpt = co_await RedisHandler::GetMsg(serverMsgID);
  if (!msgDataOpt.has_value()) {
    msgDataOpt = co_await MongoHandler::GetMsgByServerMsgIDAsync(serverMsgID);
  }

  if (!msgDataOpt.has_value()) {
    throw std::runtime_error(
        std::format("PushHandler: GetMsg Failed, serverMsgID={}", serverMsgID));
  }

  sdkws::MsgData msgData = msgDataOpt.value();

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
  ape::otel::InjectTraceContextToGrpcMetadata(ctx);

  auto status = co_await ape::grpc_util::GrpcAwait([&](auto &&handler) {
    pushStub_->async()->PushMsg(&ctx, &req, &resp,
                                std::forward<decltype(handler)>(handler));
  });

  if (!status.ok()) {
    // 处理失败，回滚消费标记，允许 Kafka 重投后重试
    co_await RedisHandler::DeleteMsgConsumed(serverMsgID);
    throw std::runtime_error(
        std::format("PushHandler: gRPC PushMsg failed: {}, serverMsgID={}",
                  status.error_message(), serverMsgID));
  } else {
    spdlog::info("PushHandler: PushMsg success, serverMsgID={}", serverMsgID);
    // 确保 Unavailable 路径处理后补写 key
    RedisHandler::TryMarkMsgConsumed(serverMsgID);
  }

  if (kafkaSpan) {
    kafkaScope.reset(); // 恢复之前的 context
    kafkaSpan->End();
  }

  co_return;
}
