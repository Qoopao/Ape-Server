#include "services/push_service/server.h"
#include "messagequeue/message_consumer.h"
#include "messagequeue/message_handler.h"
#include "services/backbon_service/client.h"
#include "services/gateway_push_service/client.h"
#include "services/push_service/push_msg_handler.h"
#include "storage/mongohandler.h"
#include "storage/mysqlhandler.h"
#include "storage/mysqlconnector.h"
#include "storage/redisconnector.h"
#include "storage/redishandler.h"
#include "storage/mysqlhandler.h"
#include <chrono>
#include <cstdint>
#include <spdlog/spdlog.h>
#include <string>
#include <sys/types.h>

PushServer::PushServer(const std::string &service_name,
                       const std::string &listen_address)
    : BaseServiceServer<PushServer>(service_name, listen_address) {}

PushServer::~PushServer() {
  if (consumer_) {
    consumer_->shutdown();
  }
  if (consumer_thread_.joinable()) {
    consumer_thread_.join();
  }
}

void PushServer::SetConsumer(std::unique_ptr<IMessageConsumer> consumer) {
  consumer_ = std::move(consumer);
}

void PushServer::SetIOCPool(IOC_Pool *ioc_pool) { ioc_pool_ = ioc_pool; }

void PushServer::Start() {

  // 先启动 gRPC 服务器（继承自 BaseServiceServer）
  BaseServiceServer<PushServer>::Start();

  // 创建 PushHandler，gRPC 调用本服务的 PushMsg
  auto pushHandler = std::make_shared<PushHandler>(grpc::CreateChannel(
      GetListenAddress(), grpc::InsecureChannelCredentials()));

  // 配置并启动消息消费者
  if (consumer_) {
    consumer_->setHandler(pushHandler);
    if (ioc_pool_) {
      consumer_->setIOCPool(ioc_pool_);
    }
    consumer_thread_ = std::thread([this]() { consumer_->start(); });
    spdlog::info("PushServer: consumer started in background thread");
  }


}

::grpc::ServerUnaryReactor *
PushServer::PushMsg(::grpc::CallbackServerContext *context,
                    const ::push::PushMsgReq *request,
                    ::push::PushMsgResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();

  // 获取发送者信息
  if (!request->has_msgdata()) {
    spdlog::error("PushServer::PushMsg: no msgdata in request");
    reactor->Finish(::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                                   "msgdata is required"));
    return reactor;
  }

  const auto msgData = request->msgdata(); // 拷贝到协程安全
  std::string conversationID = request->conversationid();

  spdlog::info(
      "[PushService] PushMsg: sender={}, recv={}, convID={}, content_type={}",
      msgData.sendid(), msgData.recvid(), conversationID,
      msgData.contenttype());

  // TODO: sender token 验证
  // 当前 PushMsg 由 msg_service (Kafka 消费链路) 内部调用，msg_service 已校验
  // sender 后续若开放外部调用，需从此处 context metadata 提取 authorization
  // token 并验证 sender
  spdlog::info(
      "[PushService] PushMsg sender={} - token validation deferred (TODO)",
      msgData.sendid());

  // 单聊前置：Outbox 已提交正文和会话，并发布正文、保存 Redis 离线数据。
  // PushMsg 只尝试在线分发；离线或网关未接受时，依靠登录同步恢复未 ACK 消息。
  // gRPC OK 表示分发处理结束，不代表客户端已收到；消费者随后记录 dispatch_done。
  // 接收完成以 MySQL ACK 回执为准，Mongo status=2 由 ACK 清理任务异步更新。
  // 新群聊使用 GroupService 的独立投递链路，本接口拒绝旧群事件。

  // 预初始化 GatewayPushClient
  getGatewayPushClient();
  GatewayPushClient *pushClient = gateway_push_client_.get();

  auto &redis = RedisConnector::instance();
  boost::asio::co_spawn(
      redis.get_executor(),
      [reactor, response, msgData = std::move(msgData),
       conversationID = std::move(conversationID),
       pushClient]() -> boost::asio::awaitable<void> {

        // GroupService owns group delivery. Reject the retired Kafka group path.
        if (msgData.isgroupmsg()) {
          reactor->Finish(grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                                      "Use GroupService Execute for group messages"));
          co_return;
        }

        // ── 单聊推送 ──
        try {
          const auto allowed = co_await MySQLHandler::WithFriendMessage(msgData,
              [&]() -> boost::asio::awaitable<void> {
                const auto recvID = msgData.recvid();
                const auto& serverMsgID = msgData.servermsgid();

                // Redis 查询异常必须向上传播：不能把依赖故障误判成用户离线，
                // 否则消费者会错误确认分发完成，停止 Outbox 的分发重试。
                const auto online = co_await RedisConnector::instance().get(
                    "user:" + std::to_string(recvID) + ":online");
                const bool recvOnline = online && (*online == "1" || *online == "true");

                bool pushed = false;
                if (recvOnline) {
                  // 在线标记只是快照，调用网关时用户可能已经断线。
                  // GatewayPushClient 对 RPC 失败也返回 false，表示本次未确认
                  // 在线投递，转由已持久化的离线恢复兜底；抛出的异常向上传播。
                  pushed = co_await pushClient->PushToUser(
                      recvID, msgData.SerializeAsString(), conversationID);
                }

                if (pushed) {
                  // 网关接受不等于客户端落盘。接收确认由 MySQL 回执记录，
                  // 无需创建旧 pending_ack Redis 元数据；未确认消息由登录恢复。
                  spdlog::info("PushService: gateway accepted message={}, receiver={}, awaiting ACK",
                               serverMsgID, recvID);
                } else {
                  // 无需再次写 dStatus、pending_ack:offline 或延迟双删正文缓存。
                  // 正文已在 Kafka 发布前保存；登录同步从 Mongo 拉取，
                  // 并用好友关系、删除水位和 MySQL ACK 回执过滤，直到接收确认。
                  spdlog::info("PushService: message={} retained for offline recovery, receiver={}, online={}",
                               serverMsgID, recvID, recvOnline);
                }

              }, true);
          if (!allowed)
            spdlog::info("PushService: discarded stale or non-friend message {}", msgData.servermsgid());
        } catch (const std::exception &e) {
          spdlog::error("PushService: single-chat dispatch failed: {}", e.what());
          reactor->Finish(::grpc::Status(grpc::StatusCode::UNAVAILABLE, "Single-chat dispatch unavailable"));
          co_return;
        }
        reactor->Finish(::grpc::Status::OK);
      },
      boost::asio::detached);

  return reactor;
}

::grpc::ServerUnaryReactor *
PushServer::AckMsg(::grpc::CallbackServerContext *context,
                   const ::push::AckMsgReq *request,
                   ::push::AckMsgResp *response) {
  auto* reactor=context->DefaultReactor();
  const auto owner=request->userid();
  const auto id=request->servermsgid();
  const auto type=request->acktype();
  boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(0),
      [reactor,owner,id,type]() -> boost::asio::awaitable<void> {
        try {
          if(type!=1 && type!=2) {
            reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,"Invalid acknowledgement type"));
            co_return;
          }
          // Both online/offline ACK persist the receipt and cleanup task atomically.
          // Returning OK does not require Redis/Mongo cleanup to finish first.
          if(!co_await MySQLHandler::QueueMessageAck(owner,id)) {
            reactor->Finish(grpc::Status(grpc::StatusCode::UNIMPLEMENTED,"Use GroupService Ack for group messages"));
            co_return;
          }
          reactor->Finish(grpc::Status::OK);
        } catch(const std::invalid_argument& e) {
          reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,e.what()));
        } catch(const std::exception& e) {
          spdlog::error("PushService.AckMsg: {}",e.what());
          reactor->Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE,"ACK persistence unavailable"));
        }
      },boost::asio::detached);
  return reactor;
}

::grpc::ServerUnaryReactor *
PushServer::AddPendingOfflineAck(::grpc::CallbackServerContext *context,
                                 const ::push::AddPendingOfflineAckReq *request,
                                 ::push::AddPendingOfflineAckResp *response) {
  auto* reactor=context->DefaultReactor();
  reactor->Finish(grpc::Status(grpc::StatusCode::UNIMPLEMENTED,"Offline ACK tracking uses durable MySQL receipts"));
  return reactor;
}

GatewayPushClient &PushServer::getGatewayPushClient() {
  if (gateway_push_client_) {
    return *gateway_push_client_;
  }

  // 延迟初始化：通过 BackbonService (etcd) 服务发现获取 GatewayPushService 地址
  std::string gateway_addr = "localhost:50055"; // fallback

  // 当前回退到硬编码 fallback 地址，后续需将 gateway_push_client_
  // 的初始化改为异步
  auto *backbon = GetBackbonClient();
  if (backbon) {
    spdlog::warn("[PushService] BackbonClient::GetServicesList is async "
                 "(co_await required), "
                 "using fallback gateway address={}",
                 gateway_addr);
  }

  gateway_channel_ =
      grpc::CreateChannel(gateway_addr, grpc::InsecureChannelCredentials());
  gateway_push_client_ = std::make_unique<GatewayPushClient>(gateway_channel_);
  return *gateway_push_client_;
}

::grpc::ServerUnaryReactor *
PushServer::DelUserPushToken(::grpc::CallbackServerContext *context,
                             const ::push::DelUserPushTokenReq *request,
                             ::push::DelUserPushTokenResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  // To-do

  reactor->Finish(::grpc::Status::OK);
  return reactor;
}
