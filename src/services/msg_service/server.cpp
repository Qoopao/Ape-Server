#include "services/msg_service/server.h"
#include "messagequeue/message_producer.h"
#include "sdkws.pb.h"
#include "util/mongohandler.h"
#include "util/otel_trace_propagation.h"
#include "util/redisconnector.h"
#include "util/redishandler.h"
#include "util/snowflake.h"
#include "util/uuid.h"
#include <spdlog/spdlog.h>
#include <string>

MsgServiceImpl::MsgServiceImpl(const std::string &service_name,
                               const std::string &listen_address)
    : BaseServiceServer<MsgServiceImpl>(service_name, listen_address) {}

void MsgServiceImpl::SetProducer(IMessageProducer *producer) {
  producer_ = producer;
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
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *
MsgServiceImpl::GetHasReadSeqs(::grpc::CallbackServerContext *context,
                               const ::msg::GetHasReadSeqsReq *request,
                               ::msg::SeqsInfoResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
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
  reactor->Finish(::grpc::Status::OK);
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
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();

  // request.seqRanges: repeated SeqRange (每个包含 conversationID, begin, end,
  // num) 对每个会话拉取 seq 在 [begin, end] 范围的消息，最多 num 条

  std::string userId = request->userid();
  auto seqRanges = request->seqranges(); // 拷贝 protobuf repeated 字段

  spdlog::info("PullMessageBySeqs: userId={}, numRanges={}", userId,
               seqRanges.size());

  // capturer 放外头以免 coroutine frame 持有 dangling 引用
  auto &redis = RedisConnector::instance();
  boost::asio::co_spawn(
      redis.get_executor(),
      [reactor, response, userId = std::move(userId),
       seqRanges = std::move(seqRanges)]() -> boost::asio::awaitable<void> {
        for (const auto &seqRange : seqRanges) {
          std::string convID = seqRange.conversationid();
          int64_t beginSeq = seqRange.begin();
          int64_t endSeq = seqRange.end();
          int32_t maxCount = seqRange.num();
          if (maxCount <= 0 || maxCount > 200)
            maxCount = 100;

          spdlog::info("PullMessageBySeqs: convID={}, begin={}, end={}, num={}",
                       convID, beginSeq, endSeq, maxCount);

          std::vector<sdkws::MsgData> messages;

          // 1. 优先从 Redis 热数据拉取（异步）
          try {
            auto redisMsgs = co_await RedisHandler::GetOfflineMsgs(
                userId, beginSeq, maxCount, convID);
            if (!redisMsgs.empty()) {
              spdlog::info("PullMessageBySeqs: convID={} pulled {} msgs from "
                           "Redis hot data",
                           convID, redisMsgs.size());
              messages = std::move(redisMsgs);
            }
          } catch (const std::exception &e) {
            spdlog::warn(
                "PullMessageBySeqs: Redis query failed for convID={}: {}",
                convID, e.what());
          }

          // 2. 热数据不足，从 MongoDB 冷数据拉取（异步，IOCPool worker 卸载）
          if (messages.empty()) {
            try {
              auto mongoMsgs =
                  co_await MongoHandler::GetMsgsBySeqFromMongoAsync(
                      userId, beginSeq, maxCount);
              spdlog::info("PullMessageBySeqs: convID={} pulled {} msgs from "
                           "MongoDB cold data",
                           convID, mongoMsgs.size());

              // 回温 user_msgs ZSET：MongoDB 冷数据写回 Redis 缓存
              // 下次拉取同一段 seq 直接命中 Redis，不再穿透 MongoDB
              for (auto &msg : mongoMsgs) {
                try {
                  co_await RedisHandler::SaveOfflineMsg(userId, msg);
                } catch (const std::exception &e) {
                  // 回温失败不影响主流程
                }
                messages.push_back(std::move(msg));
              }
            } catch (const std::exception &e) {
              spdlog::error(
                  "PullMessageBySeqs: MongoDB query failed for convID={}: {}",
                  convID, e.what());
            }
          }

          // 将该会话的消息写入响应（通过 MessageUnion 包装）
          ::sdkws::PullMsgs pullMsgs;
          for (auto &msgData : messages) {
            auto *mu = pullMsgs.add_msgs(); // MessageUnion*
            mu->set_iscmd(false);
            *mu->mutable_msg() = std::move(msgData);
          }
          pullMsgs.set_isend(messages.size() < static_cast<size_t>(maxCount));
          if (!messages.empty()) {
            pullMsgs.set_endseq(messages.back().seq());
          }
          (*response->mutable_msgs())[convID] = std::move(pullMsgs);
        }

        reactor->Finish(::grpc::Status::OK);
      },
      boost::asio::detached);

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

          // 群聊消息处理逻辑
          if (msg.isgroupmsg()) {
            // TODO: 群聊消息需要 fan-out：
            //   1. convID = groupID（群聊 convID 就是群 ID）
            //   2. 查询群成员列表
            //   3. 对每个成员分别走单聊路径（ZADD user_msgs + produce 到 MQ）
            //   4. PushMsg 端同理需要对每个成员做在线推送/离线存储
            spdlog::warn("SendMessages: group message not implemented, msg={}",
                         msg.servermsgid());
            respInfos[index]->set_errorcode("1");
            respInfos[index]->set_errormsg("group message not supported yet");
            continue;
          }

          // 单聊消息处理逻辑
          std::string normalizedConvID =
              (msg.sendid() < msg.recvid()) ? msg.sendid() + "_" + msg.recvid()
                                            : msg.recvid() + "_" + msg.sendid();
          msg.set_convid(normalizedConvID);

          // 为响应生成服务器消息id
          msg.set_servermsgid(uuid::newone_str());

          // 使用 Snowflake 生成递增唯一的会话内seq
          msg.set_seq(Snowflake::instance().nextId());

          // 将消息存入数据库
          try {
            bool saveResult = co_await MongoHandler::SaveMsgToMongoAsync(msg);
            if (!saveResult) {
              respInfos[index]->set_errorcode("1");
              respInfos[index]->set_errormsg("SaveMsgToDB failed");
              continue;
            }
          } catch (const std::exception &e) {
            spdlog::error("SendMessages: SaveMsgToDB failed for msg {}: {}",
                          msg.servermsgid(), e.what());
            respInfos[index]->set_errorcode("1");
            respInfos[index]->set_errormsg("SaveMsgToDB failed");
            continue;
          }

          // 将消息放入缓存
          try {
            bool saveResult = co_await RedisHandler::SaveMsg(msg);
            if (!saveResult) {
              respInfos[index]->set_errorcode("1");
              respInfos[index]->set_errormsg("SaveMsgInfo failed");
              continue;
            }
          } catch (const std::exception &e) {
            spdlog::error("SendMessages: SaveMsgInfo failed for msg {}: {}",
                          msg.servermsgid(), e.what());
            respInfos[index]->set_errorcode("1");
            respInfos[index]->set_errormsg("SaveMsgInfo failed");
            continue;
          }

          // 全量消息 ZADD 进 user_msgs:{recvID} ZSET（在线/离线统一）
          // score=seq，member=base64(MsgData)，TTL 7天
          // 后续 PullMessageBySeqs 和 pullAndPushOfflineMsgs 均可从这里拉取
          try {
            co_await RedisHandler::SaveOfflineMsg(msg.recvid(), msg);
          } catch (const std::exception &e) {
            spdlog::error(
                "SendMessages: SaveOfflineMsg(ZADD) failed for msg {}: {}",
                msg.servermsgid(), e.what());
            // 不影响主流程，MongoDB 冷存储兜底
          }

          // 如果会话不存在就创建会话，后续这里应该要归入会话服务
          try {
            bool exists =
                co_await MongoHandler::DoesConversationExistAsync(msg.convid());
            if (!exists) {
              co_await MongoHandler::CreateSingleChatConversationsAsync(
                  msg.sendid(), msg.recvid(), msg.convid());
            }
          } catch (const std::exception &e) {
            spdlog::error(
                "SendMessages: conversation init failed for convID={}: {}",
                msg.convid(), e.what());
          }

          // 投递到消息队列，单聊群聊的逻辑不同
          try {
            std::string tp = ape::otel::EncodeCurrentTraceParent();
            std::string payload =
                tp.empty() ? msg.servermsgid() : tp + "|" + msg.servermsgid();
            std::string key_copy = msg.convid();
            bool sendResult =
                producer->deliver(key_copy, payload.data(), payload.size());
            if (!sendResult) {
              respInfos[index]->set_errorcode("1");
              respInfos[index]->set_errormsg("MsgToMQ failed");
              continue;
            }
          } catch (const std::exception &e) {
            spdlog::error("SendMessages: MsgToMQ failed for msg {}: {}",
                          msg.servermsgid(), e.what());
            respInfos[index]->set_errorcode("1");
            respInfos[index]->set_errormsg("MsgToMQ failed");
            continue;
          }

          // 设置返回消息
          respInfos[index]->mutable_msg()->CopyFrom(msg);
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
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *MsgServiceImpl::SetConversationHasReadSeq(
    ::grpc::CallbackServerContext *context,
    const ::msg::SetConversationHasReadSeqReq *request,
    ::msg::SetConversationHasReadSeqResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *MsgServiceImpl::GetConversationsHasReadAndMaxSeq(
    ::grpc::CallbackServerContext *context,
    const ::msg::GetConversationsHasReadAndMaxSeqReq *request,
    ::msg::GetConversationsHasReadAndMaxSeqResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
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
  reactor->Finish(::grpc::Status::OK);
  return reactor;
}

::grpc::ServerUnaryReactor *MsgServiceImpl::SetUserConversationMaxSeq(
    ::grpc::CallbackServerContext *context,
    const ::msg::SetUserConversationMaxSeqReq *request,
    ::msg::SetUserConversationMaxSeqResp *response) {
  grpc::ServerUnaryReactor *reactor = context->DefaultReactor();
  reactor->Finish(::grpc::Status::OK);
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
