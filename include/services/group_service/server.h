#ifndef GROUP_SERVICE_SERVER_H
#define GROUP_SERVICE_SERVER_H

#include "groupchat.grpc.pb.h"
#include "groupchat.pb.h"
#include "services/base_service.h"
#include "services/gateway_router.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/status.h>
#include <memory>
#include <string>

class IMessageProducer;
class GroupOutboxWorker;

// GroupService v3：
//   250 Execute —— 成员/元数据/申请命令（旧 Send/Pull/Ack/Read 返回 UNSUPPORTED_OPERATION）
//   246 SendGroupMessages —— 成员锁 → stream seq → staging+outbox(op=4)，提交即返回
//   245 GetGroupChatHistory —— 无锁读 Mongo，按 published_seq 截断
// 群推送不在这里扇出：outbox worker 发 group_topic，由 PushService GroupFanout 消费。
class GroupServiceImpl final : public groupchat::GroupService::CallbackService,
                               public BaseServiceServer<GroupServiceImpl> {
public:
  GroupServiceImpl(const std::string &service_name,
                   const std::string &listen_address);
  ~GroupServiceImpl() override;

  ::grpc::ServerUnaryReactor *Execute(::grpc::CallbackServerContext* context,
      const groupchat::Command* request, groupchat::Reply* response) override;
  ::grpc::ServerUnaryReactor *SendGroupMessages(::grpc::CallbackServerContext* context,
      const groupchat::SendGroupMessageReq* request,
      groupchat::SendGroupMessageResp* response) override;
  ::grpc::ServerUnaryReactor *GetGroupChatHistory(::grpc::CallbackServerContext* context,
      const groupchat::GetGroupChatHistoryReq* request,
      groupchat::GetGroupChatHistoryResp* response) override;

  // KafkaProducer(group_topic)，必须在 Start() 之前注入。
  void SetProducer(IMessageProducer *producer);
  void StartOutbox();
  void StopOutbox();

private:
  // 成员/元数据变更后的刷新/解散/被踢通知：best-effort，不阻塞命令响应。
  boost::asio::awaitable<void> Notify(const groupchat::Command &command,
                                      const groupchat::Reply &reply);

  // account → 网关实例（best-effort 扇出，不广播、不重试）。
  GatewayRouter router_;
  IMessageProducer *producer_{nullptr};
  std::unique_ptr<GroupOutboxWorker> outbox_;
};

#endif
