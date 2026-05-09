#include "services/push_service/server.h"

PushServer::PushServer(const std::string &service_name,
                       const std::string &listen_address)
    : BaseServiceServer<PushServer>(service_name, listen_address) {}

::grpc::Status PushServer::PushMsg(::grpc::ServerContext *context,
                                   const ::push::PushMsgReq *request,
                                   ::push::PushMsgResp *response) {
  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status PushServer::DelUserPushToken(::grpc::ServerContext *context,
                                            const ::push::DelUserPushTokenReq *request,
                                            ::push::DelUserPushTokenResp *response) {
  // TO-DO
  return ::grpc::Status::OK;
}