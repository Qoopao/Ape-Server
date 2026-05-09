#ifndef PUSH_SERVICE_SERVER_H
#define PUSH_SERVICE_SERVER_H

#include "push.grpc.pb.h"
#include "push.pb.h"
#include "services/base_service.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/status.h>
#include <memory>
#include <spdlog/spdlog.h>

class PushServer final : public push::PushService::Service,
                         public BaseServiceServer<PushServer> {
public:
  PushServer(const std::string &service_name,
             const std::string &listen_address);
  ~PushServer() override = default;

  ::grpc::Status PushMsg(::grpc::ServerContext *context,
                         const ::push::PushMsgReq *request,
                         ::push::PushMsgResp *response) override;
  ::grpc::Status DelUserPushToken(::grpc::ServerContext *context,
                                  const ::push::DelUserPushTokenReq *request,
                                  ::push::DelUserPushTokenResp *response) override;
};

#endif