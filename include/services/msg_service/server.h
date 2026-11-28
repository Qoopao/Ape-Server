#ifndef MSG_SERVICE_SERVER_H
#define MSG_SERVICE_SERVER_H

#include "msg.grpc.pb.h"
#include "msg.pb.h"
#include "sdkws.pb.h"
#include "services/base_service.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/status.h>
#include <memory>
#include <spdlog/spdlog.h>

class IMessageProducer;
class OutboxWorker;

class MsgServiceImpl final : public msg::MessageService::CallbackService,
                             public BaseServiceServer<MsgServiceImpl> {
public:
  MsgServiceImpl(const std::string &service_name,
                 const std::string &listen_address);
  ~MsgServiceImpl() override;

  void StartOutbox();
  void StopOutbox();
  void SetProducer(IMessageProducer *producer);

  ::grpc::ServerUnaryReactor *GetSingleChatHistory(
      ::grpc::CallbackServerContext *context,
      const ::msg::GetSingleChatHistoryReq *request,
      ::msg::GetSingleChatHistoryResp *response) override;
  ::grpc::ServerUnaryReactor *SendMessages(
      ::grpc::CallbackServerContext *context,
      const ::sdkws::SendMessageReq *request,
      ::sdkws::SendMessageResp *response) override;

private:
  IMessageProducer *producer_{nullptr};
  std::unique_ptr<OutboxWorker> m_outbox;
};

#endif
