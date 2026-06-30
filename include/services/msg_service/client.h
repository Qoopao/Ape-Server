#ifndef MSG_SERVICE_CLIENT_H
#define MSG_SERVICE_CLIENT_H

#include "msg.grpc.pb.h"
#include "msg.pb.h"
#include <grpcpp/channel.h>
#include <boost/asio/awaitable.hpp>
#include <memory>
#include <string>

class MsgClient {
 public:
  MsgClient(std::shared_ptr<grpc::Channel> channel)
      : stub_(msg::MessageService::NewStub(channel)) {}

  boost::asio::awaitable<::sdkws::GetMaxSeqResp> GetMaxSeq(const ::sdkws::GetMaxSeqReq& request);
  boost::asio::awaitable<::msg::SeqsInfoResp> GetMaxSeqs(const ::msg::GetMaxSeqsReq& request);
  boost::asio::awaitable<::msg::SeqsInfoResp> GetHasReadSeqs(const ::msg::GetHasReadSeqsReq& request);
  boost::asio::awaitable<::msg::GetMsgByConversationIDsResp> GetMsgByConversationIDs(const ::msg::GetMsgByConversationIDsReq& request);
  boost::asio::awaitable<::msg::GetConversationMaxSeqResp> GetConversationMaxSeq(const ::msg::GetConversationMaxSeqReq& request);
  boost::asio::awaitable<::sdkws::PullMessageBySeqsResp> PullMessageBySeqs(const ::sdkws::PullMessageBySeqsReq& request);
  boost::asio::awaitable<::msg::GetSeqMessageResp> GetSeqMessage(const ::msg::GetSeqMessageReq& request);
  boost::asio::awaitable<::msg::SearchMessageResp> SearchMessage(const ::msg::SearchMessageReq& request);
  boost::asio::awaitable<::sdkws::SendMessageResp> SendMessages(const ::sdkws::SendMessageReq& request);
  boost::asio::awaitable<::msg::SendSimpleMsgResp> SendSimpleMsg(const ::msg::SendSimpleMsgReq& request);
  boost::asio::awaitable<::msg::SetUserConversationsMinSeqResp> SetUserConversationsMinSeq(const ::msg::SetUserConversationsMinSeqReq& request);
  boost::asio::awaitable<::msg::ClearConversationsMsgResp> ClearConversationsMsg(const ::msg::ClearConversationsMsgReq& request);
  boost::asio::awaitable<::msg::UserClearAllMsgResp> UserClearAllMsg(const ::msg::UserClearAllMsgReq& request);
  boost::asio::awaitable<::msg::DeleteMsgsResp> DeleteMsgs(const ::msg::DeleteMsgsReq& request);
  boost::asio::awaitable<::msg::DeleteMsgPhysicalBySeqResp> DeleteMsgPhysicalBySeq(const ::msg::DeleteMsgPhysicalBySeqReq& request);
  boost::asio::awaitable<::msg::DeleteMsgPhysicalResp> DeleteMsgPhysical(const ::msg::DeleteMsgPhysicalReq& request);
  boost::asio::awaitable<::msg::SetSendMsgStatusResp> SetSendMsgStatus(const ::msg::SetSendMsgStatusReq& request);
  boost::asio::awaitable<::msg::GetSendMsgStatusResp> GetSendMsgStatus(const ::msg::GetSendMsgStatusReq& request);
  boost::asio::awaitable<::msg::RevokeMsgResp> RevokeMsg(const ::msg::RevokeMsgReq& request);
  boost::asio::awaitable<::msg::MarkMsgsAsReadResp> MarkMsgsAsRead(const ::msg::MarkMsgsAsReadReq& request);
  boost::asio::awaitable<::msg::MarkConversationAsReadResp> MarkConversationAsRead(const ::msg::MarkConversationAsReadReq& request);
  boost::asio::awaitable<::msg::SetConversationHasReadSeqResp> SetConversationHasReadSeq(const ::msg::SetConversationHasReadSeqReq& request);
  boost::asio::awaitable<::msg::GetConversationsHasReadAndMaxSeqResp> GetConversationsHasReadAndMaxSeq(const ::msg::GetConversationsHasReadAndMaxSeqReq& request);
  boost::asio::awaitable<::msg::GetActiveUserResp> GetActiveUser(const ::msg::GetActiveUserReq& request);
  boost::asio::awaitable<::msg::GetActiveGroupResp> GetActiveGroup(const ::msg::GetActiveGroupReq& request);
  boost::asio::awaitable<::msg::GetServerTimeResp> GetServerTime(const ::msg::GetServerTimeReq& request);
  boost::asio::awaitable<::msg::ClearMsgResp> ClearMsg(const ::msg::ClearMsgReq& request);
  boost::asio::awaitable<::msg::DestructMsgsResp> DestructMsgs(const ::msg::DestructMsgsReq& request);
  boost::asio::awaitable<::msg::GetActiveConversationResp> GetActiveConversation(const ::msg::GetActiveConversationReq& request);
  boost::asio::awaitable<::msg::SetUserConversationMaxSeqResp> SetUserConversationMaxSeq(const ::msg::SetUserConversationMaxSeqReq& request);
  boost::asio::awaitable<::msg::SetUserConversationMinSeqResp> SetUserConversationMinSeq(const ::msg::SetUserConversationMinSeqReq& request);
  boost::asio::awaitable<::msg::GetLastMessageSeqByTimeResp> GetLastMessageSeqByTime(const ::msg::GetLastMessageSeqByTimeReq& request);
  boost::asio::awaitable<::msg::GetLastMessageResp> GetLastMessage(const ::msg::GetLastMessageReq& request);

 private:
  std::unique_ptr<msg::MessageService::Stub> stub_;
};

#endif