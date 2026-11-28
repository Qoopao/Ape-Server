#include "services/msg_service/server.h"
#include "messagequeue/message_producer.h"
#include "om/otel_trace_propagation.h"
#include "om/trace_context.h"
#include "sdkws.pb.h"
#include "services/msg_service/outbox_worker.h"
#include "storage/mongohandler.h"
#include "storage/mysqlhandler.h"
#include "storage/redisconnector.h"
#include "util/uuid.h"
#include <algorithm>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <chrono>
#include <cstdint>
#include <google/protobuf/util/json_util.h>
#include <limits>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <vector>

MsgServiceImpl::MsgServiceImpl(const std::string &service_name,
                               const std::string &listen_address)
    : BaseServiceServer<MsgServiceImpl>(service_name, listen_address) {}

void MsgServiceImpl::SetProducer(IMessageProducer *producer) {
  if (producer_ != producer)
    StopOutbox();
  producer_ = producer;
}

::grpc::ServerUnaryReactor *MsgServiceImpl::GetSingleChatHistory(
    ::grpc::CallbackServerContext *context,
    const ::msg::GetSingleChatHistoryReq *request,
    ::msg::GetSingleChatHistoryResp *response) {
  auto *reactor = context->DefaultReactor();
  auto query = *request;
  boost::asio::co_spawn(
      RedisConnector::instance().get_executor(),
      [reactor, response,
       query = std::move(query)]() -> boost::asio::awaitable<void> {
        if (!query.owneruserid() || !query.peeruserid() ||
            query.owneruserid() == query.peeruserid() || query.afterseq() < 0) {
          reactor->Finish(
              ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                             "Invalid single-chat history request"));
          co_return;
        }
        try {
          sdkws::MsgData candidate;
          candidate.set_sendid(query.owneruserid());
          candidate.set_recvid(query.peeruserid());
          candidate.set_seq(std::numeric_limits<int64_t>::max());
          const auto id =
              std::to_string(
                  std::min(query.owneruserid(), query.peeruserid())) +
              "_" +
              std::to_string(std::max(query.owneruserid(), query.peeruserid()));
          const int limit = std::clamp(query.limit(), 1, 100);
          response->set_peeruserid(query.peeruserid());
          const bool allowed = co_await MySQLHandler::WithFriendMessage(
              candidate,
              [response, id, query, limit]() -> boost::asio::awaitable<void> {
                const auto cutoff = co_await MySQLHandler::FriendDeletionSeq(
                    query.owneruserid(), query.peeruserid());
                auto messages =
                    co_await MongoHandler::GetSingleChatHistoryAsync(
                        id, std::max(query.afterseq(), cutoff), limit + 1);
                co_await MySQLHandler::FilterVisibleMessages(messages);
                response->set_hasmore(messages.size() >
                                      static_cast<size_t>(limit));
                for (size_t i = 0;
                     i < std::min(messages.size(), static_cast<size_t>(limit));
                     ++i) {
                  const auto &message = messages[i];
                  if ((message.sendid() != query.owneruserid() &&
                       message.sendid() != query.peeruserid()) ||
                      (message.recvid() != query.owneruserid() &&
                       message.recvid() != query.peeruserid()))
                    continue;
                  *response->add_messages() = message;
                  response->set_nextseq(message.seq());
                }
              });
          reactor->Finish(
              allowed ? ::grpc::Status::OK
                      : ::grpc::Status(::grpc::StatusCode::PERMISSION_DENIED,
                                       "Users are not friends"));
        } catch (const std::exception &e) {
          spdlog::error("GetSingleChatHistory: {}", e.what());
          reactor->Finish(::grpc::Status(::grpc::StatusCode::INTERNAL,
                                         "History unavailable"));
        }
      },
      boost::asio::detached);
  return reactor;
}

::grpc::ServerUnaryReactor *
MsgServiceImpl::SendMessages(::grpc::CallbackServerContext *context,
                             const ::sdkws::SendMessageReq *request,
                             ::sdkws::SendMessageResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();

  int msgSize = request->msgs_size();
  if (msgSize == 0) {
    reactor->Finish(
        ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "msgs is empty"));
    return reactor;
  }

  // 单条消息失败不影响同批其他消息，错误放在 infos[i]。
  std::vector<::sdkws::SendMessageRespInfo *> respInfos(msgSize);
  for (int index = 0; index < msgSize; index++) {
    respInfos[index] = response->add_infos();
    respInfos[index]->set_errorcode("0");
    respInfos[index]->set_errormsg("");
  }

  std::vector<::sdkws::MsgData> msgs;
  msgs.reserve(msgSize);
  for (int i = 0; i < msgSize; i++)
    msgs.push_back(request->msgs(i));

  // handler 会 co_spawn 到 Redis executor，线程本地 trace scope 不可用。
  const auto trace =
      ape::otel::TraceContext::FromMetadata(context->client_metadata());
  auto span = ape::otel::StartChildSpan(
      trace, "MsgService/SendMessages",
      {{"rpc.system", "grpc"}, {"im.msg_count", std::to_string(msgSize)}},
      opentelemetry::trace::SpanKind::kServer);
  const auto span_trace = span.context();

  auto &redis = RedisConnector::instance();
  IMessageProducer *producer = producer_;
  boost::asio::co_spawn(
      redis.get_executor(),
      [reactor, response, respInfos = std::move(respInfos),
       msgs = std::move(msgs), producer, span = std::move(span),
       span_trace]() mutable -> boost::asio::awaitable<void> {
        int msgSize = static_cast<int>(msgs.size());

        for (int index = 0; index < msgSize; index++) {
          ::sdkws::MsgData msg = msgs[index];

          msg.set_convid(std::to_string(std::min(msg.sendid(), msg.recvid())) +
                         "_" +
                         std::to_string(std::max(msg.sendid(), msg.recvid())));
          msg.set_servermsgid(uuid::newone_str());
          msg.set_sendtime(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count());
          try {
            auto task = co_await MySQLHandler::QueueSingleMessage(
                msg, span_trace.traceparent);
            boost::asio::steady_timer timer(
                co_await boost::asio::this_coro::executor);
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(8);

            while (task.stage == 0 && task.state != 3 &&
                   std::chrono::steady_clock::now() < deadline) {
              timer.expires_after(std::chrono::milliseconds(200));
              co_await timer.async_wait(boost::asio::use_awaitable);
              task = co_await MySQLHandler::GetOutboxTask(task.id);
            }

            if (!google::protobuf::util::JsonStringToMessage(task.payload, &msg)
                     .ok()) {
              throw std::runtime_error("Invalid stored message");
            }

            if (task.state == 3) {
              respInfos[index]->set_errorcode("NOT_FRIEND");
              respInfos[index]->set_errormsg(
                  "Message cancelled because the friend relationship was removed");
              continue;
            }

            if (task.stage == 0) {
              respInfos[index]->set_errorcode("PENDING");
              respInfos[index]->set_errormsg(
                  "Message accepted; persistence is being retried");
            }
          } catch (const std::invalid_argument &e) {
            respInfos[index]->set_errorcode("INVALID_MESSAGE");
            respInfos[index]->set_errormsg(e.what());
            continue;
          } catch (const std::exception &e) {
            spdlog::error("SendMessages: single-chat storage failed: {}",
                          e.what());
            respInfos[index]->set_errorcode("DB_ERROR");
            respInfos[index]->set_errormsg(
                "Unable to confirm submission; retry with the same clientMsgID");
            continue;
          }

          respInfos[index]->mutable_msg()->CopyFrom(msg);
        }
        span.EndOk();
        reactor->Finish(::grpc::Status::OK);
      },
      boost::asio::detached);

  return reactor;
}

MsgServiceImpl::~MsgServiceImpl() {
  Stop();
  StopOutbox();
}

// outbox：持久化待投递任务，由本服务的后台 worker 投递到 MQ。
void MsgServiceImpl::StartOutbox() {
  if (m_outbox)
    return;

  if (!producer_)
    throw std::runtime_error("MsgService producer is required before starting outbox");

  m_outbox = std::make_unique<OutboxWorker>(
      *producer_, [this]() { return GetBackbonClient(); });
  m_outbox->Start();
}

void MsgServiceImpl::StopOutbox() {
  if (m_outbox) {
    m_outbox->Stop();
    m_outbox.reset();
  }
}
