#include "services/msg_service/server.h"
#include "services/msg_service/client.h"
#include <etcd/SyncClient.hpp>
#include <string>

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

  // 检查非空
  if (request->msgs_size() == 0) {
    return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                          "msgs is empty");
  }

  //
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
