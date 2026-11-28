#ifndef GATEWAY_PUSH_SERVICE_SERVER_H
#define GATEWAY_PUSH_SERVICE_SERVER_H

#include "gateway_push.grpc.pb.h"
#include "gateway_push.pb.h"
#include "services/base_service.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>
#include <string>

// GatewayPushServer - Gateway 进程内部 gRPC 服务
// 由 Gateway 进程暴露，供 PushService / GroupService / MsgService 调用。
// 它只是 WSSessionManager 的 RPC 门面：不持有状态，必须与网关同进程。
class GatewayPushServer final
    : public gateway_push::GatewayPushService::CallbackService,
      public BaseServiceServer<GatewayPushServer> {
public:
  GatewayPushServer(const std::string &service_name,
                    const std::string &listen_address);

  ::grpc::ServerUnaryReactor *
  PushFrame(::grpc::CallbackServerContext *context,
            const ::gateway_push::PushFrameReq *request,
            ::gateway_push::PushFrameResp *response) override;

  ::grpc::ServerUnaryReactor *
  PushBatch(::grpc::CallbackServerContext *context,
            const ::gateway_push::PushBatchReq *request,
            ::gateway_push::PushBatchResp *response) override;

  ::grpc::ServerUnaryReactor *
  IsOnline(::grpc::CallbackServerContext *context,
           const ::gateway_push::IsOnlineReq *request,
           ::gateway_push::IsOnlineResp *response) override;

  ::grpc::ServerUnaryReactor *
  KickUser(::grpc::CallbackServerContext *context,
           const ::gateway_push::KickUserReq *request,
           ::gateway_push::KickUserResp *response) override;
};

#endif
