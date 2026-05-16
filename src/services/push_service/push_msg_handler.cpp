#include "services/push_service/push_msg_handler.h"
#include "sdkws.pb.h"
#include "util/otel_tracer.h"
#include "util/otel_trace_propagation.h"
#include "util/redishandler.h"
#include "util/mongohandler.h"
#include <opentelemetry/common/attribute_value.h>
#include <opentelemetry/trace/scope.h>
#include <spdlog/spdlog.h>

PushHandler::PushHandler(std::shared_ptr<grpc::Channel> pushChannel)
    : pushStub_(push::PushService::NewStub(pushChannel)) {}

void PushHandler::handle(const std::string& topic, const std::string& msg) {
    // 解析 Kafka 消息: "traceparent|servermsgid" 或 仅 "servermsgid"
    std::string traceparent, serverMsgID;
    auto sep = msg.find('|');
    if (sep != std::string::npos) {
        traceparent = msg.substr(0, sep);
        serverMsgID = msg.substr(sep + 1);
    } else {
        serverMsgID = msg;
    }

    spdlog::info("PushHandler: received msgid from Kafka, topic={}, serverMsgID={}, has_tp={}",
                 topic, serverMsgID, !traceparent.empty());

    // 1. 从Redis获取完整消息数据
    auto msgDataOpt = RedisHandler::GetMsgInfo(serverMsgID);
    if (!msgDataOpt.has_value()) {
        spdlog::error("PushHandler: GetMsgInfo from Redis failed, serverMsgID={}", serverMsgID);
        return;
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
            traceparent, "Kafka Consume " + topic,
            kafkaAttrs,
            tracer);
        if (kafkaSpan) {
            kafkaScope = std::make_unique<opentelemetry::trace::Scope>(kafkaSpan);
        }
    }

    // 2. 构造PushMsgReq并调用PushService
    push::PushMsgReq req;
    *req.mutable_msgdata() = msgData;
    if (!msgData.convid().empty()) {
        req.set_conversationid(msgData.convid());
    } else {
        req.set_conversationid(msgData.sendid() + "_" + msgData.recvid());
    }

    push::PushMsgResp resp;
    grpc::ClientContext ctx;

    // 注入 traceparent 到 gRPC metadata，让 PushService 拦截器提取
    ape::otel::InjectTraceContextToGrpcMetadata(ctx);

    auto status = pushStub_->PushMsg(&ctx, req, &resp);
    if (!status.ok()) {
        spdlog::error("PushHandler: gRPC PushMsg failed: {}, serverMsgID={}",
                      status.error_message(), serverMsgID);
    } else {
        spdlog::info("PushHandler: PushMsg success, serverMsgID={}", serverMsgID);
    }

    if (kafkaSpan) {
        kafkaScope.reset();  // 恢复之前的 context
        kafkaSpan->End();
    }
}
