#ifndef PUSH_SERVICE_SERVER_H
#define PUSH_SERVICE_SERVER_H

#include "messagequeue/message_consumer.h"
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

// 前向声明
class IOC_Pool;
class RedisHandler;
class GatewayPushClient;

class PushServer final : public push::PushService::CallbackService,
                         public BaseServiceServer<PushServer> {
public:
  PushServer(const std::string &service_name,
             const std::string &listen_address);
  ~PushServer() override;

  // 依赖注入：设置消息消费者和 IOC_Pool（在 Start() 之前调用）
  void SetConsumer(std::unique_ptr<IMessageConsumer> consumer);
  void SetIOCPool(IOC_Pool *ioc_pool);

  // 启动 gRPC 服务器和消息消费
  void Start();

  ::grpc::ServerUnaryReactor * PushMsg(::grpc::CallbackServerContext *context,
                         const ::push::PushMsgReq *request,
                         ::push::PushMsgResp *response) override;
  ::grpc::ServerUnaryReactor * DelUserPushToken(::grpc::CallbackServerContext *context,
                                  const ::push::DelUserPushTokenReq *request,
                                  ::push::DelUserPushTokenResp *response) override;
  ::grpc::ServerUnaryReactor * AckMsg(::grpc::CallbackServerContext *context,
                        const ::push::AckMsgReq *request,
                        ::push::AckMsgResp *response) override;
  ::grpc::ServerUnaryReactor * AddPendingOfflineAck(::grpc::CallbackServerContext *context,
                                      const ::push::AddPendingOfflineAckReq *request,
                                      ::push::AddPendingOfflineAckResp *response) override;

private:
  // 延迟初始化 GatewayPushClient（通过 etcd 服务发现 GatewayPushService 地址）
  GatewayPushClient& getGatewayPushClient();

  // 后台协程：定期扫描超时的在线 pending_ack，回退到离线存储
  boost::asio::awaitable<void> CheckExpiredPendingAck();

  std::unique_ptr<IMessageConsumer> consumer_;
  std::thread consumer_thread_;

  IOC_Pool *ioc_pool_{nullptr};

  // gRPC 推送到 Gateway 的客户端
  std::shared_ptr<grpc::Channel> gateway_channel_;
  std::unique_ptr<GatewayPushClient> gateway_push_client_;
};

#endif
