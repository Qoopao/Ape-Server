#include "services/msg_service/server.h"
#include "sdkws.pb.h"
#include <etcd/SyncClient.hpp>
#include <string>
#include "util/uuid.h"
#include <spdlog/spdlog.h>

// 类外定义
std::atomic<bool> MsgServiceImpl::should_exit(false);
std::condition_variable MsgServiceImpl::cv_quit;
std::mutex MsgServiceImpl::mtx_quit;

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

  // TO-DO
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

    // 生成会话的orderIndex，从数据库中找应该是多少，这里先随便设一个
    msg.set_seq(0);

    // 将消息存到数据库

    // 如果没有会话，那么创建会话

    // 转发消息到MQ

    // 设置返回消息
    respInfos[index]->mutable_msg()->CopyFrom(msg);

  }
  // 测试用，打印一下request的全部字段
  spdlog::info("Request msgs size: {}", request->msgs_size());
  for(int i = 0; i < request->msgs_size(); i++){
    const ::sdkws::MsgData& msg = request->msgs(i);
    spdlog::info("Request msg[{}]: sendid={}, recvid={}, convid={}, clientmsgid={}",
                i, msg.sendid(), msg.recvid(), msg.convid(), msg.clientmsgid());
    spdlog::info("  Content: {}, sessiontype={}, msgfrom={}, contentType={}, sendtime={}",
                msg.content(), msg.sessiontype(), msg.msgfrom(), msg.contenttype(), msg.sendtime());
  }

  // 测试用，打印一下response的全部字段
  spdlog::info("Response infos size: {}", response->infos_size());
  for(int i = 0; i < response->infos_size(); i++){
    const ::sdkws::SendMessageRespInfo& info = response->infos(i);
    spdlog::info("Response info[{}]: errorcode={}, errormsg={}", 
                i, info.errorcode(), info.errormsg());
    
    if(info.has_msg()){
      const ::sdkws::MsgData& msg = info.msg();
      spdlog::info("  Msg: sendid={}, recvid={}, convid={}, clientmsgid={}, servermsgid={}",
                  msg.sendid(), msg.recvid(), msg.convid(), msg.clientmsgid(), msg.servermsgid());
      spdlog::info("  Content: {}, sessiontype={}, msgfrom={}, contentType={}, sendtime={}",
                  msg.content(), msg.sessiontype(), msg.msgfrom(), msg.contenttype(), msg.sendtime());
    }
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
