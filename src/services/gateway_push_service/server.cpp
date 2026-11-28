#include "services/gateway_push_service/server.h"
#include "gateway/ws_session_manager.h"
#include "om/obs_metrics.h"
#include "om/trace_context.h"
#include "sdkws.pb.h"

#include <spdlog/spdlog.h>

GatewayPushServer::GatewayPushServer(const std::string &service_name,
                                     const std::string &listen_address)
    : BaseServiceServer<GatewayPushServer>(service_name, listen_address) {}

::grpc::ServerUnaryReactor *GatewayPushServer::PushFrame(
    ::grpc::CallbackServerContext *context,
    const ::gateway_push::PushFrameReq *request,
    ::gateway_push::PushFrameResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  response->set_instance_id(GetInstanceId());

  const auto trace = ape::otel::TraceContext::FromMetadata(context->client_metadata());
  auto span = ape::otel::StartChildSpan(
      trace, "GatewayPush/PushFrame",
      {{"rpc.system", "grpc"}, {"ws.type", std::to_string(request->type())}},
      opentelemetry::trace::SpanKind::kServer);

  const uint64_t account = request->account();
  if (account == 0) {
    span.EndError("empty_account");
    spdlog::warn("GatewayPushServer::PushFrame: empty account");
    response->set_success(false);
    reactor->Finish(
        ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "account required"));
    return reactor;
  }

  // success=true 的语义严格限定为：目标 session 在本实例且帧已入写队列
  sdkws::SdkWSResp frame;
  frame.set_type(request->type());
  frame.set_account(account);
  frame.set_data(request->data());
  if (!request->request_id().empty())
    frame.set_requestid(request->request_id());
  if (!request->error_code().empty())
    frame.set_errorcode(request->error_code());
  // 让客户端能拿推送帧反查投递 trace。
  frame.set_logid(span.context().traceId());

  const bool pushed = WSSessionManager::instance().pushToUser(
      account, std::make_shared<std::string>(frame.SerializeAsString()));
  response->set_success(pushed);
  if (pushed)
    span.EndOk();
  else
    span.Attribute("push.result", "not_online").EndOk();
  spdlog::debug("GatewayPushServer::PushFrame: account={}, type={}, pushed={}",
                account, request->type(), pushed);
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *GatewayPushServer::PushBatch(
    ::grpc::CallbackServerContext *context,
    const ::gateway_push::PushBatchReq *request,
    ::gateway_push::PushBatchResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  response->set_instance_id(GetInstanceId());

  const int32_t type = request->type();
  for (const auto account : request->accounts()) {
    if (account == 0) {
      response->add_results(false);
      continue;
    }
    sdkws::SdkWSResp frame;
    frame.set_type(type);
    frame.set_account(account);
    frame.set_data(request->data());
    response->add_results(WSSessionManager::instance().pushToUser(
        account, std::make_shared<std::string>(frame.SerializeAsString())));
  }
  spdlog::debug("GatewayPushServer::PushBatch: accounts={}, type={}",
                request->accounts_size(), type);
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *GatewayPushServer::IsOnline(
    ::grpc::CallbackServerContext *context,
    const ::gateway_push::IsOnlineReq *request,
    ::gateway_push::IsOnlineResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  response->set_instance_id(GetInstanceId());
  response->set_online(
      WSSessionManager::instance().hasSession(request->account()));
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *GatewayPushServer::KickUser(
    ::grpc::CallbackServerContext *context,
    const ::gateway_push::KickUserReq *request,
    ::gateway_push::KickUserResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  const std::string self = GetInstanceId();
  response->set_instance_id(self);

  // except_instance_id 是发起方自己，避免同一实例内自踢
  if (!request->except_instance_id().empty() &&
      request->except_instance_id() == self) {
    response->set_kicked(false);
    reactor->Finish(::grpc::Status::OK);
    return reactor;
  }

  const std::string reason =
      request->reason().empty() ? "duplicate_login" : request->reason();
  const bool kicked =
      WSSessionManager::instance().kickUser(request->account(), reason);
  response->set_kicked(kicked);
  spdlog::info("GatewayPushServer::KickUser: account={}, reason={}, kicked={}",
               request->account(), reason, kicked);
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}
