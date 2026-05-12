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
class GatewayPushClient;

class PushServer final : public push::PushService::Service,
                         public BaseServiceServer<PushServer> {
public:
  PushServer(const std::string &service_name,
             const std::string &listen_address);
  ~PushServer() override;

  // 启动Kafka消费者，开始监听消息推送
  void Start(const std::string &kafka_group_id,
             const std::vector<std::string> &kafka_topics,
             const std::vector<std::string> &offline_topics = {});

  ::grpc::Status PushMsg(::grpc::ServerContext *context,
                         const ::push::PushMsgReq *request,
                         ::push::PushMsgResp *response) override;
  ::grpc::Status DelUserPushToken(::grpc::ServerContext *context,
                                  const ::push::DelUserPushTokenReq *request,
                                  ::push::DelUserPushTokenResp *response) override;
  ::grpc::Status AckMsg(::grpc::ServerContext *context,
                        const ::push::AckMsgReq *request,
                        ::push::AckMsgResp *response) override;
  ::grpc::Status AddPendingOfflineAck(::grpc::ServerContext *context,
                                      const ::push::AddPendingOfflineAckReq *request,
                                      ::push::AddPendingOfflineAckResp *response) override;

private:
  // 延迟初始化 GatewayPushClient（通过 etcd 服务发现 GatewayPushService 地址）
  GatewayPushClient& getGatewayPushClient();

  std::unique_ptr<KafkaConsumer> kafkaConsumer_;
  std::thread kafka_thread_;  // Kafka 消费线程（在线推送）

  std::unique_ptr<KafkaConsumer> offlineConsumer_;
  std::thread offline_thread_;  // Kafka 消费线程（离线持久化）

  // gRPC 推送到 Gateway 的客户端
  std::shared_ptr<grpc::Channel> gateway_channel_;
  std::unique_ptr<GatewayPushClient> gateway_push_client_;
};

#endif
