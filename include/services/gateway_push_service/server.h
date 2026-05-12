#ifndef GATEWAY_PUSH_SERVICE_SERVER_H
#define GATEWAY_PUSH_SERVICE_SERVER_H

#include "gateway_push.grpc.pb.h"
#include "gateway_push.pb.h"
#include "services/base_service.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/status.h>
#include <spdlog/spdlog.h>
#include <string>

// GatewayPushServer - Gateway 进程内部 gRPC 服务
// 由 Gateway 进程暴露，供 PushService 等独立微服务调用
// 负责将消息通过 WebSocket 长连接推送给指定在线用户
class GatewayPushServer final : public gateway_push::GatewayPushService::Service,
                                public BaseServiceServer<GatewayPushServer> {
public:
    GatewayPushServer(const std::string &service_name,
                      const std::string &listen_address);

    ::grpc::Status PushToUser(::grpc::ServerContext *context,
                              const ::gateway_push::PushToUserReq *request,
                              ::gateway_push::PushToUserResp *response) override;
};

#endif