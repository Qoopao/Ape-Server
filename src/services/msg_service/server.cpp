#include "services/msg_service/server.h"
#include "services/msg_service/outbox_worker.h"
#include "messagequeue/message_producer.h"
#include "om/otel_trace_propagation.h"
#include "sdkws.pb.h"
#include "storage/mongohandler.h"
#include "storage/mysqlhandler.h"
#include "storage/redisconnector.h"
#include "storage/redishandler.h"
#include "util/snowflake.h"
#include "util/uuid.h"
#include <cstdint>
#include <spdlog/spdlog.h>
#include <string>
#include <chrono>
#include <stdexcept>
#include <limits>
#include <google/protobuf/util/json_util.h>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>

MsgServiceImpl::MsgServiceImpl(const std::string &service_name,
                               const std::string &listen_address)
    : BaseServiceServer<MsgServiceImpl>(service_name, listen_address) {}

void MsgServiceImpl::SetProducer(IMessageProducer *producer) {
  if(producer_!=producer) StopOutbox();
  producer_ = producer;
}

::grpc::ServerUnaryReactor *MsgServiceImpl::GetSingleChatHistory(
    ::grpc::CallbackServerContext *context,
    const ::msg::GetSingleChatHistoryReq *request,
    ::msg::GetSingleChatHistoryResp *response) {
  auto *reactor = context->DefaultReactor();
  auto query = *request;
  boost::asio::co_spawn(RedisConnector::instance().get_executor(),
      [reactor, response, query = std::move(query)]() -> boost::asio::awaitable<void> {
        if (!query.owneruserid() || !query.peeruserid() ||
            query.owneruserid() == query.peeruserid() || query.afterseq() < 0) {
          reactor->Finish(::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
              "Invalid single-chat history request"));
          co_return;
        }
        try {
          sdkws::MsgData candidate;
          candidate.set_sendid(query.owneruserid());
          candidate.set_recvid(query.peeruserid());
          candidate.set_seq(std::numeric_limits<int64_t>::max());
          const auto id = std::to_string(std::min(query.owneruserid(), query.peeruserid())) +
              "_" + std::to_string(std::max(query.owneruserid(), query.peeruserid()));
          const int limit = std::clamp(query.limit(), 1, 100);
          response->set_peeruserid(query.peeruserid());
          const bool allowed = co_await MySQLHandler::WithFriendMessage(candidate,
              [response, id, query, limit]() -> boost::asio::awaitable<void> {
                const auto cutoff = co_await MySQLHandler::FriendDeletionSeq(query.owneruserid(), query.peeruserid());
                auto messages = co_await MongoHandler::GetSingleChatHistoryAsync(
                    id, std::max(query.afterseq(), cutoff), limit + 1);
                co_await MySQLHandler::FilterVisibleMessages(messages);
                response->set_hasmore(messages.size() > static_cast<size_t>(limit));
                for (size_t i = 0; i < std::min(messages.size(), static_cast<size_t>(limit)); ++i) {
                  const auto& message = messages[i];
                  if ((message.sendid() != query.owneruserid() &&
                       message.sendid() != query.peeruserid()) ||
                      (message.recvid() != query.owneruserid() &&
                       message.recvid() != query.peeruserid()))
                    continue;
                  *response->add_messages() = message;
                  response->set_nextseq(message.seq());
                }
              });
          reactor->Finish(allowed ? ::grpc::Status::OK :
              ::grpc::Status(::grpc::StatusCode::PERMISSION_DENIED, "Users are not friends"));
        } catch (const std::exception& e) {
          spdlog::error("GetSingleChatHistory: {}", e.what());
          reactor->Finish(::grpc::Status(::grpc::StatusCode::INTERNAL, "History unavailable"));
        }
      }, boost::asio::detached);
  return reactor;
}

// ──────────────────────────────────────────
// TO-DO stubs (未实现，保持原样)
// ──────────────────────────────────────────

::grpc::ServerUnaryReactor *
MsgServiceImpl::GetMaxSeq(::grpc::CallbackServerContext *context,
                          const ::sdkws::GetMaxSeqReq *request,
                          ::sdkws::GetMaxSeqResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *
MsgServiceImpl::GetMaxSeqs(::grpc::CallbackServerContext *context,
                           const ::msg::GetMaxSeqsReq *request,
                           ::msg::SeqsInfoResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "Use ConversationService"));
  return reactor;
}

::grpc::ServerUnaryReactor *
MsgServiceImpl::GetHasReadSeqs(::grpc::CallbackServerContext *context,
                               const ::msg::GetHasReadSeqsReq *request,
                               ::msg::SeqsInfoResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "Use ConversationService"));
  return reactor;
}

::grpc::ServerUnaryReactor *MsgServiceImpl::GetMsgByConversationIDs(
    ::grpc::CallbackServerContext *context,
    const ::msg::GetMsgByConversationIDsReq *request,
    ::msg::GetMsgByConversationIDsResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *MsgServiceImpl::GetConversationMaxSeq(
    ::grpc::CallbackServerContext *context,
    const ::msg::GetConversationMaxSeqReq *request,
    ::msg::GetConversationMaxSeqResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "Use ConversationService"));
  return reactor;
}

// ──────────────────────────────────────────
// PullMessageBySeqs: 拉取消息
// 通过 co_spawn 异步调用 RedisHandler 和 RedisConnector
// ──────────────────────────────────────────
::grpc::ServerUnaryReactor *
MsgServiceImpl::PullMessageBySeqs(::grpc::CallbackServerContext *context,
                                  const ::sdkws::PullMessageBySeqsReq *request,
                                  ::sdkws::PullMessageBySeqsResp *response) {
  auto* reactor=context->DefaultReactor();
  reactor->Finish(grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
      "Use GetSingleChatHistory or GroupService Execute; legacy Redis offline pull is retired"));
  return reactor;
}

::grpc::ServerUnaryReactor *
MsgServiceImpl::GetSeqMessage(::grpc::CallbackServerContext *context,
                              const ::msg::GetSeqMessageReq *request,
                              ::msg::GetSeqMessageResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *
MsgServiceImpl::SearchMessage(::grpc::CallbackServerContext *context,
                              const ::msg::SearchMessageReq *request,
                              ::msg::SearchMessageResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

// ──────────────────────────────────────────
// SendMessages: 发送消息
// 通过 co_spawn 异步调用 RedisHandler
// ──────────────────────────────────────────
::grpc::ServerUnaryReactor *
MsgServiceImpl::SendMessages(::grpc::CallbackServerContext *context,
                             const ::sdkws::SendMessageReq *request,
                             ::sdkws::SendMessageResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();

  int msgSize = request->msgs_size();
  // 检查非空
  if (msgSize == 0) {
    reactor->Finish(
        ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "msgs is empty"));
    return reactor;
  }

  // 注意，如果其中一步出错了，直接继续处理下一条消息，并不返回调用错误，因为不是调用错误
  std::vector<::sdkws::SendMessageRespInfo *> respInfos(msgSize);
  for (int index = 0; index < msgSize; index++) {
    // 构造返回信息, 默认成功
    respInfos[index] = response->add_infos();
    respInfos[index]->set_errorcode("0");
    respInfos[index]->set_errormsg("");
  }

  // 拷贝所有消息到协程
  std::vector<::sdkws::MsgData> msgs;
  msgs.reserve(msgSize);
  for (int i = 0; i < msgSize; i++) {
    msgs.push_back(request->msgs(i));
  }

  auto &redis = RedisConnector::instance();
  IMessageProducer *producer = producer_;
  boost::asio::co_spawn(
      redis.get_executor(),
      [reactor, response, respInfos = std::move(respInfos),
       msgs = std::move(msgs), producer]() -> boost::asio::awaitable<void> {
        int msgSize = static_cast<int>(msgs.size());

        for (int index = 0; index < msgSize; index++) {
          ::sdkws::MsgData msg = msgs[index];

          // 群聊消息转移到网关判断，不在这里判断了
          if (msg.isgroupmsg()) {
            respInfos[index]->set_errorcode("UPGRADE_REQUIRED");
            respInfos[index]->set_errormsg("Use group chat Send (type 250)");
            continue;
          } else {
            msg.set_convid(std::to_string(std::min(msg.sendid(), msg.recvid())) + "_" +
                std::to_string(std::max(msg.sendid(), msg.recvid())));
            msg.set_servermsgid(uuid::newone_str());
            msg.set_seq(Snowflake::instance().nextId());
            msg.set_sendtime(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            try {
              auto task = co_await MySQLHandler::QueueSingleMessage(msg);
              // 设置超时时间
              boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
              const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
              
              // 任务尚未完成 && 未取消 && 当前时间 < deadline
              while (task.stage == 0 && task.state != 3 && std::chrono::steady_clock::now() < deadline) {
                timer.expires_after(std::chrono::milliseconds(200));
                co_await timer.async_wait(boost::asio::use_awaitable);
                task = co_await MySQLHandler::GetOutboxTask(task.id);
              }

              if (!google::protobuf::util::JsonStringToMessage(task.payload, &msg).ok())
                throw std::runtime_error("Invalid stored message");
              if (task.state == 3) {
                respInfos[index]->set_errorcode("NOT_FRIEND");
                respInfos[index]->set_errormsg("Message cancelled because the friend relationship was removed");
                continue;
              }
              if (task.stage == 0) {
                respInfos[index]->set_errorcode("PENDING");
                respInfos[index]->set_errormsg("Message accepted; persistence is being retried");
              }
            } catch (const std::invalid_argument& e) {
              respInfos[index]->set_errorcode("INVALID_MESSAGE");
              respInfos[index]->set_errormsg(e.what());
              continue;
            } catch (const std::exception &e) {
              spdlog::error("SendMessages: single-chat storage failed: {}", e.what());
              respInfos[index]->set_errorcode("DB_ERROR");
              respInfos[index]->set_errormsg("Unable to confirm submission; retry with the same clientMsgID");
              continue;
            }

            // 设置返回消息
            respInfos[index]->mutable_msg()->CopyFrom(msg);
          }

        }
        reactor->Finish(::grpc::Status::OK);
      },
      boost::asio::detached);

  return reactor;
}

::grpc::ServerUnaryReactor *
MsgServiceImpl::SendSimpleMsg(::grpc::CallbackServerContext *context,
                              const ::msg::SendSimpleMsgReq *request,
                              ::msg::SendSimpleMsgResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *MsgServiceImpl::SetUserConversationsMinSeq(
    ::grpc::CallbackServerContext *context,
    const ::msg::SetUserConversationsMinSeqReq *request,
    ::msg::SetUserConversationsMinSeqResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *MsgServiceImpl::ClearConversationsMsg(
    ::grpc::CallbackServerContext *context,
    const ::msg::ClearConversationsMsgReq *request,
    ::msg::ClearConversationsMsgResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *
MsgServiceImpl::UserClearAllMsg(::grpc::CallbackServerContext *context,
                                const ::msg::UserClearAllMsgReq *request,
                                ::msg::UserClearAllMsgResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *
MsgServiceImpl::DeleteMsgs(::grpc::CallbackServerContext *context,
                           const ::msg::DeleteMsgsReq *request,
                           ::msg::DeleteMsgsResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *MsgServiceImpl::DeleteMsgPhysicalBySeq(
    ::grpc::CallbackServerContext *context,
    const ::msg::DeleteMsgPhysicalBySeqReq *request,
    ::msg::DeleteMsgPhysicalBySeqResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *
MsgServiceImpl::DeleteMsgPhysical(::grpc::CallbackServerContext *context,
                                  const ::msg::DeleteMsgPhysicalReq *request,
                                  ::msg::DeleteMsgPhysicalResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *
MsgServiceImpl::SetSendMsgStatus(::grpc::CallbackServerContext *context,
                                 const ::msg::SetSendMsgStatusReq *request,
                                 ::msg::SetSendMsgStatusResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *
MsgServiceImpl::GetSendMsgStatus(::grpc::CallbackServerContext *context,
                                 const ::msg::GetSendMsgStatusReq *request,
                                 ::msg::GetSendMsgStatusResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *
MsgServiceImpl::RevokeMsg(::grpc::CallbackServerContext *context,
                          const ::msg::RevokeMsgReq *request,
                          ::msg::RevokeMsgResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *
MsgServiceImpl::MarkMsgsAsRead(::grpc::CallbackServerContext *context,
                               const ::msg::MarkMsgsAsReadReq *request,
                               ::msg::MarkMsgsAsReadResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *MsgServiceImpl::MarkConversationAsRead(
    ::grpc::CallbackServerContext *context,
    const ::msg::MarkConversationAsReadReq *request,
    ::msg::MarkConversationAsReadResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "Use ConversationService"));
  return reactor;
}

::grpc::ServerUnaryReactor *MsgServiceImpl::SetConversationHasReadSeq(
    ::grpc::CallbackServerContext *context,
    const ::msg::SetConversationHasReadSeqReq *request,
    ::msg::SetConversationHasReadSeqResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "Use ConversationService"));
  return reactor;
}

::grpc::ServerUnaryReactor *MsgServiceImpl::GetConversationsHasReadAndMaxSeq(
    ::grpc::CallbackServerContext *context,
    const ::msg::GetConversationsHasReadAndMaxSeqReq *request,
    ::msg::GetConversationsHasReadAndMaxSeqResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "Use ConversationService"));
  return reactor;
}

::grpc::ServerUnaryReactor *
MsgServiceImpl::GetActiveUser(::grpc::CallbackServerContext *context,
                              const ::msg::GetActiveUserReq *request,
                              ::msg::GetActiveUserResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *
MsgServiceImpl::GetActiveGroup(::grpc::CallbackServerContext *context,
                               const ::msg::GetActiveGroupReq *request,
                               ::msg::GetActiveGroupResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *
MsgServiceImpl::GetServerTime(::grpc::CallbackServerContext *context,
                              const ::msg::GetServerTimeReq *request,
                              ::msg::GetServerTimeResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *
MsgServiceImpl::ClearMsg(::grpc::CallbackServerContext *context,
                         const ::msg::ClearMsgReq *request,
                         ::msg::ClearMsgResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *
MsgServiceImpl::DestructMsgs(::grpc::CallbackServerContext *context,
                             const ::msg::DestructMsgsReq *request,
                             ::msg::DestructMsgsResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *MsgServiceImpl::GetActiveConversation(
    ::grpc::CallbackServerContext *context,
    const ::msg::GetActiveConversationReq *request,
    ::msg::GetActiveConversationResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "Use ConversationService"));
  return reactor;
}

::grpc::ServerUnaryReactor *MsgServiceImpl::SetUserConversationMaxSeq(
    ::grpc::CallbackServerContext *context,
    const ::msg::SetUserConversationMaxSeqReq *request,
    ::msg::SetUserConversationMaxSeqResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "Use ConversationService"));
  return reactor;
}

::grpc::ServerUnaryReactor *MsgServiceImpl::SetUserConversationMinSeq(
    ::grpc::CallbackServerContext *context,
    const ::msg::SetUserConversationMinSeqReq *request,
    ::msg::SetUserConversationMinSeqResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *MsgServiceImpl::GetLastMessageSeqByTime(
    ::grpc::CallbackServerContext *context,
    const ::msg::GetLastMessageSeqByTimeReq *request,
    ::msg::GetLastMessageSeqByTimeResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *
MsgServiceImpl::GetLastMessage(::grpc::CallbackServerContext *context,
                               const ::msg::GetLastMessageReq *request,
                               ::msg::GetLastMessageResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

MsgServiceImpl::~MsgServiceImpl() { Stop(); StopOutbox(); }
void MsgServiceImpl::StartOutbox() {
    if(m_outbox) return;
    if(!producer_) throw std::runtime_error("MsgService producer is required before starting outbox");
    m_outbox=std::make_unique<OutboxWorker>(*producer_);
    m_outbox->Start();
}
void MsgServiceImpl::StopOutbox() {
    if(m_outbox) { m_outbox->Stop(); m_outbox.reset(); }
}
