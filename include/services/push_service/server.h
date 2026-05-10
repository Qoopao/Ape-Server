#ifndef PUSH_SERVICE_SERVER_H
#define PUSH_SERVICE_SERVER_H

#include "push.grpc.pb.h"
#include "push.pb.h"
#include "services/base_service.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/status.h>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>
#include <vector>

// 前向声明
class KafkaConsumer;
class RedisHandler;

class PushServer final : public push::PushService::Service,
                         public BaseServiceServer<PushServer> {
public:
  PushServer(const std::string &service_name,
             const std::string &listen_address);
  ~PushServer() override;

  // 启动Kafka消费者，开始监听消息推送
  void Start(const std::string &kafka_group_id,
             const std::vector<std::string> &kafka_topics);

  ::grpc::Status PushMsg(::grpc::ServerContext *context,
                         const ::push::PushMsgReq *request,
                         ::push::PushMsgResp *response) override;
  ::grpc::Status DelUserPushToken(::grpc::ServerContext *context,
                                  const ::push::DelUserPushTokenReq *request,
                                  ::push::DelUserPushTokenResp *response) override;

private:
  std::unique_ptr<KafkaConsumer> kafkaConsumer_;
  std::thread kafka_thread_;  // Kafka 消费线程
};

#endif
