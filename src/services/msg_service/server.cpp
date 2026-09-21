#include "services/msg_service/server.h"
#include "messagequeue/message_producer.h"
#include "om/otel_trace_propagation.h"
#include "sdkws.pb.h"
#include "storage/mongohandler.h"
#include "storage/mysqlhandler.h"
#include "storage/redisconnector.h"
#include "storage/redishandler.h"
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

          // 2b. 冷数据不足时，额外拉取群消息（此用户所在群的群消息）
          if (messages.empty()) {
            try {
              auto groupIDs = co_await MySQLHandler::GetUserGroupIDs(userId);
              if (!groupIDs.empty()) {
                auto groupMsgs =
                    co_await MongoHandler::GetGroupMsgsBySeqFromMongoAsync(
                        groupIDs, beginSeq, maxCount);
                if (!groupMsgs.empty()) {
                  spdlog::info("PullMessageBySeqs: convID={} pulled {} group "
                               "msgs from MongoDB cold data",
                               convID, groupMsgs.size());
                  for (auto &msg : groupMsgs) {
                    try {
                      co_await RedisHandler::SaveOfflineMsg(userId, msg);
                    } catch (...) {
                    }
                    messages.push_back(std::move(msg));
                  }
                  // 按 seq 排序并截断
                  std::sort(messages.begin(), messages.end(),
                            [](const auto &a, const auto &b) {
                              return a.seq() < b.seq();
                            });
                  if (static_cast<int>(messages.size()) > maxCount) {
                    messages.resize(maxCount);
                  }
                }
              }
            } catch (const std::exception &e) {
              spdlog::warn(
                  "PullMessageBySeqs: group msg query failed for {}: {}",
                  userId, e.what());
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

          // ── 群聊 / 单聊 分流 ──
          if (msg.isgroupmsg()) {
            // convID 就是 groupID（客户端传入）
            std::string groupID = msg.convid();
            msg.set_convid(groupID);
            msg.set_recvid(groupID);

            // 生成服务器消息 ID 和序列号
            msg.set_servermsgid(uuid::newone_str());
            msg.set_seq(Snowflake::instance().nextId());

            // 存入 MongoDB
            if (!co_await MongoHandler::SaveMsgToMongoAsync(msg)) {
              respInfos[index]->set_errorcode("1");
              respInfos[index]->set_errormsg("SaveMsgToDB failed");
              continue;
            }

            // 存入 Redis 消息缓存
            if (!co_await RedisHandler::SaveMsg(msg)) {
              continue;
            }

            // 获取群成员列表（Redis 缓存优先，MySQL 兜底）
            std::vector<std::string> memberIDs;
            memberIDs =
                co_await RedisHandler::GetGroupMembersFromCache(groupID);
            if (memberIDs.empty()) {
              auto members =
                  co_await MySQLHandler::GetGroupMembers(groupID, 0, 10000);
              for (const auto &m : members) {
                memberIDs.push_back(m.userid());
              }
              if (!memberIDs.empty()) {
                co_await RedisHandler::CacheGroupMembers(groupID, memberIDs);
              }
            }

            // 批量 fan-out 到每个成员的 user_msgs ZSET（Lua EVAL）
            if (!memberIDs.empty()) {
              co_await RedisHandler::BatchSaveOfflineMsg(memberIDs, msg);
            }

            // 确保群会话存在
            bool exists =
                co_await MongoHandler::DoesConversationExistAsync(groupID);
            if (!exists) {
              co_await MongoHandler::CreateGroupChatConversationsAsync(
                  groupID, memberIDs);
            }

            // 大小群分流：小群投递 Kafka 走实时推送，大群纯 pull
            // 阈值与 PushService::PushMsg 保持一致（200）
            if (!memberIDs.empty() && memberIDs.size() <= 200) {
              try {
                std::string tp = ape::otel::EncodeCurrentTraceParent();
                std::string payload = tp.empty() ? msg.servermsgid()
                                                 : tp + "|" + msg.servermsgid();
                std::string key_copy = msg.convid();
                bool ok =
                    producer->deliver(key_copy, payload.data(), payload.size());
                if (!ok) {
                  spdlog::warn("SendMessages(group): Kafka deliver failed for "
                               "small group, msg={}, group={}",
                               msg.servermsgid(), groupID);
                }
              } catch (const std::exception &e) {
                spdlog::warn(
                    "SendMessages(group): Kafka deliver exception for {}, {}",
                    msg.servermsgid(), e.what());
              }
            }
          } else {
            // ── 单聊消息处理逻辑 ──
            std::string normalizedConvID =
                (msg.sendid() < msg.recvid())
                    ? msg.sendid() + "_" + msg.recvid()
                    : msg.recvid() + "_" + msg.sendid();
            msg.set_convid(normalizedConvID);

            // 为响应生成服务器消息id
            msg.set_servermsgid(uuid::newone_str());

            // 使用 Snowflake 生成递增唯一的会话内seq
            msg.set_seq(Snowflake::instance().nextId());

            // 将消息存入数据库
            bool saveResult = co_await MongoHandler::SaveMsgToMongoAsync(msg);
            if (!saveResult) {
              respInfos[index]->set_errorcode("1");
              respInfos[index]->set_errormsg("SaveMsgToDB failed");
              continue;
            }

            // 将消息放入缓存
            bool saveRedisResult = co_await RedisHandler::SaveMsg(msg);
            if (!saveRedisResult) {
              respInfos[index]->set_errorcode("1");
              respInfos[index]->set_errormsg("SaveMsgInfo failed");
              continue;
            }

            // 全量消息 ZADD 进 user_msgs:{recvID} ZSET（在线/离线统一）
            // score=seq，member=base64(MsgData)，TTL 7天
            // 后续 PullMessageBySeqs 和 pullAndPushOfflineMsgs 均可从这里拉取
            co_await RedisHandler::SaveOfflineMsg(msg.recvid(), msg);

            // 如果会话不存在就创建会话，后续这里应该要归入会话服务
            bool exists =
                co_await MongoHandler::DoesConversationExistAsync(msg.convid());
            if (!exists) {
              co_await MongoHandler::CreateSingleChatConversationsAsync(
                  msg.sendid(), msg.recvid(), msg.convid());
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
        }
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
