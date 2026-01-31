#ifndef MSG_SERVICE_CLIENT_H
#define MSG_SERVICE_CLIENT_H

#include "msg.grpc.pb.h"
#include "msg.pb.h"
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <google/protobuf/empty.pb.h>


class MsgClientHandler  {
public:
    MsgClientHandler(std::shared_ptr<::grpc::Channel> channel): channel(channel){
        this->stub = std::make_unique<::msg::MessageService::Stub>(channel);
    };
    ~MsgClientHandler() = default;

    void GetMaxSeqClient(const ::sdkws::GetMaxSeqReq& request, ::sdkws::GetMaxSeqResp& response);
    void GetMaxSeqsClient(const ::msg::GetMaxSeqsReq& request, ::msg::SeqsInfoResp& response);
    void GetHasReadSeqsClient(const ::msg::GetHasReadSeqsReq& request, ::msg::SeqsInfoResp& response);
    void GetMsgByConversationIDsClient(const ::msg::GetMsgByConversationIDsReq& request, ::msg::GetMsgByConversationIDsResp& response);
    void GetConversationMaxSeqClient(const ::msg::GetConversationMaxSeqReq& request, ::msg::GetConversationMaxSeqResp& response);
    void PullMessageBySeqsClient(const ::sdkws::PullMessageBySeqsReq& request, ::sdkws::PullMessageBySeqsResp& response);
    void GetSeqMessageClient(const ::msg::GetSeqMessageReq& request, ::msg::GetSeqMessageResp& response);
    void SearchMessageClient(const ::msg::SearchMessageReq& request, ::msg::SearchMessageResp& response);
    void SendMessagesClient(const ::sdkws::SendMessageReq& request, ::sdkws::SendMessageResp& response);
    void SendSimpleMsgClient(const ::msg::SendSimpleMsgReq& request, ::msg::SendSimpleMsgResp& response);
    void SetUserConversationsMinSeqClient(const ::msg::SetUserConversationsMinSeqReq& request, ::msg::SetUserConversationsMinSeqResp& response);
    void ClearConversationsMsgClient(const ::msg::ClearConversationsMsgReq& request, ::msg::ClearConversationsMsgResp& response);
    void UserClearAllMsgClient(const ::msg::UserClearAllMsgReq& request, ::msg::UserClearAllMsgResp& response);
    void DeleteMsgsClient(const ::msg::DeleteMsgsReq& request, ::msg::DeleteMsgsResp& response);
    void DeleteMsgPhysicalBySeqClient(const ::msg::DeleteMsgPhysicalBySeqReq& request, ::msg::DeleteMsgPhysicalBySeqResp& response);
    void DeleteMsgPhysicalClient(const ::msg::DeleteMsgPhysicalReq& request, ::msg::DeleteMsgPhysicalResp& response);
    void SetSendMsgStatusClient(const ::msg::SetSendMsgStatusReq& request, ::msg::SetSendMsgStatusResp& response);
    void GetSendMsgStatusClient(const ::msg::GetSendMsgStatusReq& request, ::msg::GetSendMsgStatusResp& response);
    void RevokeMsgClient(const ::msg::RevokeMsgReq& request, ::msg::RevokeMsgResp& response);
    void MarkMsgsAsReadClient(const ::msg::MarkMsgsAsReadReq& request, ::msg::MarkMsgsAsReadResp& response);
    void MarkConversationAsReadClient(const ::msg::MarkConversationAsReadReq& request, ::msg::MarkConversationAsReadResp& response);
    void SetConversationHasReadSeqClient(const ::msg::SetConversationHasReadSeqReq& request, ::msg::SetConversationHasReadSeqResp& response);
    void GetConversationsHasReadAndMaxSeqClient(const ::msg::GetConversationsHasReadAndMaxSeqReq& request, ::msg::GetConversationsHasReadAndMaxSeqResp& response);
    void GetActiveUserClient(const ::msg::GetActiveUserReq& request, ::msg::GetActiveUserResp& response);
    void GetActiveGroupClient(const ::msg::GetActiveGroupReq& request, ::msg::GetActiveGroupResp& response);
    void GetServerTimeClient(const ::msg::GetServerTimeReq& request, ::msg::GetServerTimeResp& response);
    void ClearMsgClient(const ::msg::ClearMsgReq& request, ::msg::ClearMsgResp& response);
    void DestructMsgsClient(const ::msg::DestructMsgsReq& request, ::msg::DestructMsgsResp& response);
    void GetActiveConversationClient(const ::msg::GetActiveConversationReq& request, ::msg::GetActiveConversationResp& response);
    void SetUserConversationMaxSeqClient(const ::msg::SetUserConversationMaxSeqReq& request, ::msg::SetUserConversationMaxSeqResp& response);
    void SetUserConversationMinSeqClient(const ::msg::SetUserConversationMinSeqReq& request, ::msg::SetUserConversationMinSeqResp& response);
    void GetLastMessageSeqByTimeClient(const ::msg::GetLastMessageSeqByTimeReq& request, ::msg::GetLastMessageSeqByTimeResp& response);
    void GetLastMessageClient(const ::msg::GetLastMessageReq& request, ::msg::GetLastMessageResp& response);

private:
    std::shared_ptr<::grpc::Channel> channel;
    std::unique_ptr<::msg::MessageService::Stub> stub;
};

#endif