#ifndef MSG_SERVICE_CLIENT_H
#define MSG_SERVICE_CLIENT_H

#include "msg.grpc.pb.h"
#include "msg.pb.h"
#include <grpcpp/channel.h>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

class MsgClient {
 public:
  MsgClient(std::shared_ptr<grpc::Channel> channel)
      : stub_(msg::MessageService::NewStub(channel)) {}

  ::sdkws::GetMaxSeqResp GetMaxSeq(const ::sdkws::GetMaxSeqReq& request);
  ::msg::SeqsInfoResp GetMaxSeqs(const ::msg::GetMaxSeqsReq& request);
  ::msg::SeqsInfoResp GetHasReadSeqs(const ::msg::GetHasReadSeqsReq& request);
  ::msg::GetMsgByConversationIDsResp GetMsgByConversationIDs(const ::msg::GetMsgByConversationIDsReq& request);
  ::msg::GetConversationMaxSeqResp GetConversationMaxSeq(const ::msg::GetConversationMaxSeqReq& request);
  ::sdkws::PullMessageBySeqsResp PullMessageBySeqs(const ::sdkws::PullMessageBySeqsReq& request);
  ::msg::GetSeqMessageResp GetSeqMessage(const ::msg::GetSeqMessageReq& request);
  ::msg::SearchMessageResp SearchMessage(const ::msg::SearchMessageReq& request);
  ::sdkws::SendMessageResp SendMessages(const ::sdkws::SendMessageReq& request);
  ::msg::SendSimpleMsgResp SendSimpleMsg(const ::msg::SendSimpleMsgReq& request);
  ::msg::SetUserConversationsMinSeqResp SetUserConversationsMinSeq(const ::msg::SetUserConversationsMinSeqReq& request);
  ::msg::ClearConversationsMsgResp ClearConversationsMsg(const ::msg::ClearConversationsMsgReq& request);
  ::msg::UserClearAllMsgResp UserClearAllMsg(const ::msg::UserClearAllMsgReq& request);
  ::msg::DeleteMsgsResp DeleteMsgs(const ::msg::DeleteMsgsReq& request);
  ::msg::DeleteMsgPhysicalBySeqResp DeleteMsgPhysicalBySeq(const ::msg::DeleteMsgPhysicalBySeqReq& request);
  ::msg::DeleteMsgPhysicalResp DeleteMsgPhysical(const ::msg::DeleteMsgPhysicalReq& request);
  ::msg::SetSendMsgStatusResp SetSendMsgStatus(const ::msg::SetSendMsgStatusReq& request);
  ::msg::GetSendMsgStatusResp GetSendMsgStatus(const ::msg::GetSendMsgStatusReq& request);
  ::msg::RevokeMsgResp RevokeMsg(const ::msg::RevokeMsgReq& request);
  ::msg::MarkMsgsAsReadResp MarkMsgsAsRead(const ::msg::MarkMsgsAsReadReq& request);
  ::msg::MarkConversationAsReadResp MarkConversationAsRead(const ::msg::MarkConversationAsReadReq& request);
  ::msg::SetConversationHasReadSeqResp SetConversationHasReadSeq(const ::msg::SetConversationHasReadSeqReq& request);
  ::msg::GetConversationsHasReadAndMaxSeqResp GetConversationsHasReadAndMaxSeq(const ::msg::GetConversationsHasReadAndMaxSeqReq& request);
  ::msg::GetActiveUserResp GetActiveUser(const ::msg::GetActiveUserReq& request);
  ::msg::GetActiveGroupResp GetActiveGroup(const ::msg::GetActiveGroupReq& request);
  ::msg::GetServerTimeResp GetServerTime(const ::msg::GetServerTimeReq& request);
  ::msg::ClearMsgResp ClearMsg(const ::msg::ClearMsgReq& request);
  ::msg::DestructMsgsResp DestructMsgs(const ::msg::DestructMsgsReq& request);
  ::msg::GetActiveConversationResp GetActiveConversation(const ::msg::GetActiveConversationReq& request);
  ::msg::SetUserConversationMaxSeqResp SetUserConversationMaxSeq(const ::msg::SetUserConversationMaxSeqReq& request);
  ::msg::SetUserConversationMinSeqResp SetUserConversationMinSeq(const ::msg::SetUserConversationMinSeqReq& request);
  ::msg::GetLastMessageSeqByTimeResp GetLastMessageSeqByTime(const ::msg::GetLastMessageSeqByTimeReq& request);
  ::msg::GetLastMessageResp GetLastMessage(const ::msg::GetLastMessageReq& request);

 private:
  std::unique_ptr<msg::MessageService::Stub> stub_;
};

#endif