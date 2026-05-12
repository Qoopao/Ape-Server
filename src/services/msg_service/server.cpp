#include "services/msg_service/server.h"
#include "sdkws.pb.h"
#include <string>
#include "util/uuid.h"
#include <spdlog/spdlog.h>
#include "util/redishandler.h"
#include "util/redisconnector.h"
#include "util/mongohandler.h"
#include "util/snowflake.h"
#include "messagequeue/kafkaproducer.h"

MsgServiceImpl::MsgServiceImpl(const std::string &service_name,
                               const std::string &listen_address)
    : BaseServiceServer<MsgServiceImpl>(service_name, listen_address) {}

::grpc::Status MsgServiceImpl::GetMaxSeq(::grpc::ServerContext *context,
                                         const ::sdkws::GetMaxSeqReq *request,
                                         ::sdkws::GetMaxSeqResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status MsgServiceImpl::GetMaxSeqs(::grpc::ServerContext *context,
                                          const ::msg::GetMaxSeqsReq *request,
                                          ::msg::SeqsInfoResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status
MsgServiceImpl::GetHasReadSeqs(::grpc::ServerContext *context,
                               const ::msg::GetHasReadSeqsReq *request,
                               ::msg::SeqsInfoResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status MsgServiceImpl::GetMsgByConversationIDs(
    ::grpc::ServerContext *context,
    const ::msg::GetMsgByConversationIDsReq *request,
    ::msg::GetMsgByConversationIDsResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status MsgServiceImpl::GetConversationMaxSeq(
    ::grpc::ServerContext *context,
    const ::msg::GetConversationMaxSeqReq *request,
    ::msg::GetConversationMaxSeqResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status
MsgServiceImpl::PullMessageBySeqs(::grpc::ServerContext *context,
                                  const ::sdkws::PullMessageBySeqsReq *request,
                                  ::sdkws::PullMessageBySeqsResp *response) {

  // PullMessageBySeqs: 拉取消息
  // request.seqRanges: repeated SeqRange (每个包含 conversationID, begin, end, num)
  // 对每个会话拉取 seq 在 [begin, end] 范围的消息，最多 num 条

  std::string userId = request->userid();
  const auto& seqRanges = request->seqranges();

  spdlog::info("PullMessageBySeqs: userId={}, numRanges={}", userId, seqRanges.size());

  for (const auto& seqRange : seqRanges) {
    std::string convID = seqRange.conversationid();
    int64_t beginSeq = seqRange.begin();
    int64_t endSeq = seqRange.end();
    int32_t maxCount = seqRange.num();
    if (maxCount <= 0 || maxCount > 200) maxCount = 100;

    spdlog::info("PullMessageBySeqs: convID={}, begin={}, end={}, num={}", convID, beginSeq, endSeq, maxCount);

    std::vector<sdkws::MsgData> messages;

    // 1. 优先从 Redis 热数据拉取
    try {
      auto redisMsgs = RedisHandler::GetOfflineMsgs(userId, beginSeq, maxCount, convID);
      if (!redisMsgs.empty()) {
        spdlog::info("PullMessageBySeqs: convID={} pulled {} msgs from Redis hot data", convID, redisMsgs.size());
        messages = std::move(redisMsgs);
      }
    } catch (const std::exception& e) {
      spdlog::warn("PullMessageBySeqs: Redis query failed for convID={}: {}", convID, e.what());
    }

    // 2. 热数据不足，从 MongoDB 冷数据拉取
    if (messages.empty()) {
      try {
        auto mongoMsgs = MongoHandler::GetOfflineMsgsFromMongo(userId, beginSeq, maxCount);
        spdlog::info("PullMessageBySeqs: convID={} pulled {} msgs from MongoDB cold data", convID, mongoMsgs.size());
        messages = std::move(mongoMsgs);
      } catch (const std::exception& e) {
        spdlog::error("PullMessageBySeqs: MongoDB query failed for convID={}: {}", convID, e.what());
      }
    }

    // 将该会话的消息写入响应（通过 MessageUnion 包装）
    ::sdkws::PullMsgs pullMsgs;
    for (auto& msgData : messages) {
      auto* mu = pullMsgs.add_msgs();  // MessageUnion*
      mu->set_iscmd(false);
      *mu->mutable_msg() = std::move(msgData);
    }
    pullMsgs.set_isend(messages.size() < static_cast<size_t>(maxCount));
    if (!messages.empty()) {
      pullMsgs.set_endseq(messages.back().seq());
    }
    (*response->mutable_msgs())[convID] = std::move(pullMsgs);
  }

  return ::grpc::Status::OK;
}

::grpc::Status
MsgServiceImpl::GetSeqMessage(::grpc::ServerContext *context,
                              const ::msg::GetSeqMessageReq *request,
                              ::msg::GetSeqMessageResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status
MsgServiceImpl::SearchMessage(::grpc::ServerContext *context,
                              const ::msg::SearchMessageReq *request,
                              ::msg::SearchMessageResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status
MsgServiceImpl::SendMessages(::grpc::ServerContext *context,
                             const ::sdkws::SendMessageReq *request,
                             ::sdkws::SendMessageResp *response) {

  int msgSize = request->msgs_size();
  // 检查非空
  if (msgSize == 0) {
    return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                          "msgs is empty");
  }

  // 注意，如果其中一步出错了，直接继续处理下一条消息，并不返回调用错误，因为不是调用错误
  std::vector<::sdkws::SendMessageRespInfo*> respInfos(msgSize);
  for(int index = 0; index < msgSize; index++){
    // 构造返回信息, 默认成功
    respInfos[index] = response->add_infos();
    respInfos[index]->set_errorcode("0");
    respInfos[index]->set_errormsg("");
  }
  
  for(int index = 0; index < msgSize; index++){
    // 拷贝
    ::sdkws::MsgData msg = request->msgs(index);
    // 如果会话id为空
    if(msg.convid().empty()){
      spdlog::error("conv_id is empty, client_msg_id: {}, send_id: {}, msg_index: {}", msg.clientmsgid(), msg.sendid(), index);
      respInfos[index]->set_errorcode("1");
      respInfos[index]->set_errormsg("conv_id is empty"); 
      continue;
    }

    // 为响应生成服务器消息id
    msg.set_servermsgid(uuid::newone_str());

    // 使用 Snowflake 生成递增唯一 seq
    msg.set_seq(Snowflake::instance().nextId());

    // 将消息存到redis
    bool saveResult = RedisHandler::SaveMsgInfo(msg);
    if(!saveResult){
      respInfos[index]->set_errorcode("1");
      respInfos[index]->set_errormsg("SaveMsgInfo failed"); 
      continue;
    }

    // TODO: 如果没有会话，那么创建会话

    // 判断接收方在线状态，路由到对应的 Kafka Topic
    bool recvOnline = false;
    try {
        auto &redis = RedisConnector::get_instance().get_redis();
        auto onlineVal = redis.get("user:" + msg.recvid() + ":online");
        recvOnline = (onlineVal && (*onlineVal == "1" || *onlineVal == "true"));
    } catch (const std::exception &e) {
        spdlog::error("SendMessages: Redis online check error for recvid={}: {}", msg.recvid(), e.what());
    }

    if (recvOnline) {
        // 在线 → 发送到 msg_topic，由 PushHandler 消费后通过 GatewayPushService 推送至长连接
        spdlog::info("SendMessages: recvid={} is online, routing to msg_topic", msg.recvid());
        bool sendResult = RedisHandler::MsgToMQ(msg.sendid(), msg.servermsgid());
        if (!sendResult) {
            respInfos[index]->set_errorcode("1");
            respInfos[index]->set_errormsg("MsgToMQ failed");
            continue;
        }
    } else {
        // 离线 → 发送到 offline_msg_topic，由 OfflineMsgHandler 消费并持久化
        spdlog::info("SendMessages: recvid={} is offline, routing to offline_msg_topic", msg.recvid());
        bool sendResult = RedisHandler::MsgToOfflineMQ(msg.sendid(), msg.servermsgid());
        if (!sendResult) {
            respInfos[index]->set_errorcode("1");
            respInfos[index]->set_errormsg("MsgToOfflineMQ failed");
            continue;
        }
    }

    // 设置返回消息
    respInfos[index]->mutable_msg()->CopyFrom(msg);

  }

  return ::grpc::Status::OK;
}

::grpc::Status
MsgServiceImpl::SendSimpleMsg(::grpc::ServerContext *context,
                              const ::msg::SendSimpleMsgReq *request,
                              ::msg::SendSimpleMsgResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status MsgServiceImpl::SetUserConversationsMinSeq(
    ::grpc::ServerContext *context,
    const ::msg::SetUserConversationsMinSeqReq *request,
    ::msg::SetUserConversationsMinSeqResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status MsgServiceImpl::ClearConversationsMsg(
    ::grpc::ServerContext *context,
    const ::msg::ClearConversationsMsgReq *request,
    ::msg::ClearConversationsMsgResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status
MsgServiceImpl::UserClearAllMsg(::grpc::ServerContext *context,
                                const ::msg::UserClearAllMsgReq *request,
                                ::msg::UserClearAllMsgResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status MsgServiceImpl::DeleteMsgs(::grpc::ServerContext *context,
                                          const ::msg::DeleteMsgsReq *request,
                                          ::msg::DeleteMsgsResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status MsgServiceImpl::DeleteMsgPhysicalBySeq(
    ::grpc::ServerContext *context,
    const ::msg::DeleteMsgPhysicalBySeqReq *request,
    ::msg::DeleteMsgPhysicalBySeqResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status
MsgServiceImpl::DeleteMsgPhysical(::grpc::ServerContext *context,
                                  const ::msg::DeleteMsgPhysicalReq *request,
                                  ::msg::DeleteMsgPhysicalResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status
MsgServiceImpl::SetSendMsgStatus(::grpc::ServerContext *context,
                                 const ::msg::SetSendMsgStatusReq *request,
                                 ::msg::SetSendMsgStatusResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status
MsgServiceImpl::GetSendMsgStatus(::grpc::ServerContext *context,
                                 const ::msg::GetSendMsgStatusReq *request,
                                 ::msg::GetSendMsgStatusResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status MsgServiceImpl::RevokeMsg(::grpc::ServerContext *context,
                                         const ::msg::RevokeMsgReq *request,
                                         ::msg::RevokeMsgResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status
MsgServiceImpl::MarkMsgsAsRead(::grpc::ServerContext *context,
                               const ::msg::MarkMsgsAsReadReq *request,
                               ::msg::MarkMsgsAsReadResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status MsgServiceImpl::MarkConversationAsRead(
    ::grpc::ServerContext *context,
    const ::msg::MarkConversationAsReadReq *request,
    ::msg::MarkConversationAsReadResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status MsgServiceImpl::SetConversationHasReadSeq(
    ::grpc::ServerContext *context,
    const ::msg::SetConversationHasReadSeqReq *request,
    ::msg::SetConversationHasReadSeqResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status MsgServiceImpl::GetConversationsHasReadAndMaxSeq(
    ::grpc::ServerContext *context,
    const ::msg::GetConversationsHasReadAndMaxSeqReq *request,
    ::msg::GetConversationsHasReadAndMaxSeqResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status
MsgServiceImpl::GetActiveUser(::grpc::ServerContext *context,
                              const ::msg::GetActiveUserReq *request,
                              ::msg::GetActiveUserResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status
MsgServiceImpl::GetActiveGroup(::grpc::ServerContext *context,
                               const ::msg::GetActiveGroupReq *request,
                               ::msg::GetActiveGroupResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status
MsgServiceImpl::GetServerTime(::grpc::ServerContext *context,
                              const ::msg::GetServerTimeReq *request,
                              ::msg::GetServerTimeResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status MsgServiceImpl::ClearMsg(::grpc::ServerContext *context,
                                        const ::msg::ClearMsgReq *request,
                                        ::msg::ClearMsgResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status
MsgServiceImpl::DestructMsgs(::grpc::ServerContext *context,
                             const ::msg::DestructMsgsReq *request,
                             ::msg::DestructMsgsResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status MsgServiceImpl::GetActiveConversation(
    ::grpc::ServerContext *context,
    const ::msg::GetActiveConversationReq *request,
    ::msg::GetActiveConversationResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status MsgServiceImpl::SetUserConversationMaxSeq(
    ::grpc::ServerContext *context,
    const ::msg::SetUserConversationMaxSeqReq *request,
    ::msg::SetUserConversationMaxSeqResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status MsgServiceImpl::SetUserConversationMinSeq(
    ::grpc::ServerContext *context,
    const ::msg::SetUserConversationMinSeqReq *request,
    ::msg::SetUserConversationMinSeqResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status MsgServiceImpl::GetLastMessageSeqByTime(
    ::grpc::ServerContext *context,
    const ::msg::GetLastMessageSeqByTimeReq *request,
    ::msg::GetLastMessageSeqByTimeResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}

::grpc::Status
MsgServiceImpl::GetLastMessage(::grpc::ServerContext *context,
                               const ::msg::GetLastMessageReq *request,
                               ::msg::GetLastMessageResp *response) {

  // TO-DO
  return ::grpc::Status::OK;
}
