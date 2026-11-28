#ifndef PUSH_SERVICE_SERVER_H
#define PUSH_SERVICE_SERVER_H

#include "messagequeue/message_consumer.h"
#include "messagequeue/message_handler.h"
#include "push.grpc.pb.h"
#include "push.pb.h"
#include "services/base_service.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>
#include <vector>

class IOC_Pool;
class RedisHandler;
class GatewayRouter;

class PushServer final : public push::PushService::CallbackService,
                         public BaseServiceServer<PushServer> {
public:
  PushServer(const std::string &service_name,
             const std::string &listen_address);
  ~PushServer() override;

  void SetConsumer(std::unique_ptr<IMessageConsumer> consumer);
  void SetGroupConsumer(std::unique_ptr<IMessageConsumer> consumer);
  void SetIOCPool(IOC_Pool *ioc_pool);
  void Start();

  ::grpc::ServerUnaryReactor *PushMsg(
      ::grpc::CallbackServerContext *context,
      const ::push::PushMsgReq *request,
      ::push::PushMsgResp *response) override;
  ::grpc::ServerUnaryReactor *AckConversations(
      ::grpc::CallbackServerContext *context,
      const ::push::AckConversationsReq *request,
      ::push::AckConversationsResp *response) override;

private:
  std::unique_ptr<IMessageConsumer> consumer_;
  std::thread consumer_thread_;
  // 群扇出：group_topic → GroupFanoutHandler → PushBatchBestEffort(251)
  std::unique_ptr<IMessageConsumer> group_consumer_;
  std::thread group_consumer_thread_;
  std::shared_ptr<MessageHandler> group_handler_;

  IOC_Pool *ioc_pool_{nullptr};
  std::unique_ptr<GatewayRouter> router_;
};

#endif
