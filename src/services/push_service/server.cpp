#include "services/push_service/server.h"
#include "messagequeue/message_consumer.h"
#include "messagequeue/message_handler.h"
#include "services/backbon_service/client.h"
#include "services/gateway_router.h"
#include "services/gateway_push_service/client.h"
#include "services/push_service/group_fanout_handler.h"
#include "services/push_service/push_msg_handler.h"
#include "storage/mysqlhandler.h"
#include "storage/mysqlconnector.h"
#include "storage/redisconnector.h"
#include "om/obs_metrics.h"
#include "om/trace_context.h"
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <spdlog/spdlog.h>
#include <string>
#include <utility>

PushServer::PushServer(const std::string &service_name,
                       const std::string &listen_address)
    : BaseServiceServer<PushServer>(service_name, listen_address) {
  const char *fallback = std::getenv("GATEWAY_PUSH_ADDR");
  router_ = std::make_unique<GatewayRouter>(
      [this]() { return GetBackbonClient(); },
      fallback && *fallback ? fallback : "localhost:50055");
}

PushServer::~PushServer() {
  if (consumer_)
    consumer_->shutdown();
  if (consumer_thread_.joinable())
    consumer_thread_.join();
  if (group_consumer_)
    group_consumer_->shutdown();
  if (group_consumer_thread_.joinable())
    group_consumer_thread_.join();
}

void PushServer::SetConsumer(std::unique_ptr<IMessageConsumer> consumer) {
  consumer_ = std::move(consumer);
}

void PushServer::SetGroupConsumer(std::unique_ptr<IMessageConsumer> consumer) {
  group_consumer_ = std::move(consumer);
}

void PushServer::SetIOCPool(IOC_Pool *ioc_pool) { ioc_pool_ = ioc_pool; }

void PushServer::Start() {
  BaseServiceServer<PushServer>::Start();

  // 单聊 msg_topic：PushHandler -> 本进程 gRPC PushMsg -> GatewayRouter。
  auto pushHandler = std::make_shared<PushHandler>(grpc::CreateChannel(
      GetListenAddress(), grpc::InsecureChannelCredentials()));
  if (consumer_) {
    consumer_->setHandler(pushHandler);
    if (ioc_pool_)
      consumer_->setIOCPool(ioc_pool_);
    consumer_thread_ = std::thread([this]() { consumer_->start(); });
    spdlog::info("PushServer: consumer started in background thread");
  }

  // 群扇出 group_topic：GroupFanoutHandler -> PushBatchBestEffort(251)。
  if (group_consumer_) {
    group_handler_ = std::make_shared<GroupFanoutHandler>(router_.get());
    group_consumer_->setHandler(group_handler_);
    if (ioc_pool_)
      group_consumer_->setIOCPool(ioc_pool_);
    group_consumer_thread_ = std::thread([this]() { group_consumer_->start(); });
    spdlog::info("PushServer: group fanout consumer started in background thread");
  }
}

::grpc::ServerUnaryReactor *
PushServer::PushMsg(::grpc::CallbackServerContext *context,
                    const ::push::PushMsgReq *request,
                    ::push::PushMsgResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();

  if (!request->has_msgdata()) {
    spdlog::error("PushServer::PushMsg: no msgdata in request");
    reactor->Finish(::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                                   "msgdata is required"));
    return reactor;
  }

  const auto msgData = request->msgdata();
  std::string conversationID = request->conversationid();

  spdlog::info(
      "[PushService] PushMsg: sender={}, recv={}, convID={}, content_type={}",
      msgData.sendid(), msgData.recvid(), conversationID,
      msgData.contenttype());

  // 单聊前置：Outbox 已提交正文和会话，并发布正文、保存 Redis 离线数据。
  // PushMsg 只尝试在线分发；离线或网关未接受时，依靠登录同步恢复未 ACK 消息。
  // gRPC OK 表示分发处理结束，不代表客户端已收到；接收完成以 MySQL ACK 回执为准。
  GatewayRouter *router = router_.get();
  if (router == nullptr) {
    spdlog::error("PushServer::PushMsg: gateway router not initialized");
    reactor->Finish(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                 "Gateway router not initialized"));
    return reactor;
  }

  const auto trace =
      ape::otel::TraceContext::FromMetadata(context->client_metadata());
  auto span = ape::otel::StartChildSpan(
      trace, "PushService/PushMsg",
      {{"rpc.system", "grpc"}, {"im.server_msg_id", msgData.servermsgid()}},
      opentelemetry::trace::SpanKind::kServer);
  const auto span_trace = span.context();

  auto &redis = RedisConnector::instance();
  boost::asio::co_spawn(
      redis.get_executor(),
      [reactor, response, msgData = std::move(msgData), router,
       span = std::move(span), span_trace]() mutable
          -> boost::asio::awaitable<void> {
        // GroupService owns group delivery; reject the retired Kafka group path.
        if (msgData.isgroupmsg()) {
          ape::obs::RecordPush("rejected");
          span.EndError("group_message_rejected");
          reactor->Finish(
              grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                           "Use GroupService Execute for group messages"));
          co_return;
        }

        try {
          // 默认视为“已判定无需投递”（非好友/过期/重复），消费者可以确认分发。
          push::PushDecision decision = push::PUSH_DISCARDED;
          const bool allowed = co_await MySQLHandler::WithFriendMessage(
              msgData,
              [&]() -> boost::asio::awaitable<void> {
                const auto recvID = msgData.recvid();
                const auto result = co_await router->PushFrame(
                    recvID, 104, msgData.SerializeAsString(), {}, {},
                    span_trace);
                switch (result) {
                  case PushResult::Delivered:
                    ape::obs::RecordPush("delivered");
                    decision = push::PUSH_DELIVERED;
                    spdlog::info("PushService: gateway accepted message={}, "
                                 "receiver={}, awaiting ACK",
                                 msgData.servermsgid(), recvID);
                    break;
                  case PushResult::NotOnline:
                    ape::obs::RecordPush("not_online");
                    decision = push::PUSH_NOT_ONLINE;
                    spdlog::info("PushService: receiver={} offline, message={} "
                                 "retained for offline recovery",
                                 recvID, msgData.servermsgid());
                    break;
                  case PushResult::Unknown:
                    ape::obs::RecordPush("unknown");
                    // 没问到网关不能当成离线：确认分发会让消息静默丢失（INV-2）。
                    throw std::runtime_error(
                        "gateway push outcome unknown; keep outbox task for retry");
                }
              },
              true);
          if (!allowed) {
            ape::obs::RecordPush("discarded");
            spdlog::info("PushService: discarded stale or non-friend message {}",
                         msgData.servermsgid());
          }
          response->set_decision(decision);
        } catch (const std::exception &e) {
          span.EndError(e.what());
          spdlog::error("PushService: single-chat dispatch failed: {}",
                        e.what());
          reactor->Finish(::grpc::Status(::grpc::StatusCode::UNAVAILABLE,
                                         "Single-chat dispatch unavailable"));
          co_return;
        }
        span.EndOk();
        reactor->Finish(::grpc::Status::OK);
      },
      boost::asio::detached);

  return reactor;
}

::grpc::ServerUnaryReactor *
PushServer::AckConversations(::grpc::CallbackServerContext *context,
                             const ::push::AckConversationsReq *request,
                             ::push::AckConversationsResp *response) {
  auto *reactor = context->DefaultReactor();
  const auto owner = request->userid();
  const auto type = request->acktype();
  std::vector<std::pair<std::string, int64_t>> acks;
  for (const auto &ack : request->conversations())
    acks.emplace_back(ack.conversationid(), ack.uptoseq());

  boost::asio::co_spawn(
      MySQLConnector::instance().get_pool_executor(0),
      [reactor, owner, type, acks = std::move(acks)]()
          -> boost::asio::awaitable<void> {
        try {
          if (type != 1 && type != 2)
            throw std::invalid_argument("Invalid acknowledgement type");
          if (acks.empty())
            throw std::invalid_argument("No conversation acknowledgement");
          for (const auto &[conversation, upToSeq] : acks)
            co_await MySQLHandler::AckConversation(owner, conversation, upToSeq);
          reactor->Finish(grpc::Status::OK);
        } catch (const std::invalid_argument &e) {
          reactor->Finish(
              grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, e.what()));
        } catch (const std::exception &e) {
          spdlog::error("PushService.AckConversations: {}", e.what());
          reactor->Finish(grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                       "ACK persistence unavailable"));
        }
      },
      boost::asio::detached);
  return reactor;
}
