#include "services/msg_service/client.h"
#include <spdlog/spdlog.h>


void MsgClientHandler::GetMaxSeqClient(const ::sdkws::GetMaxSeqReq& request, ::sdkws::GetMaxSeqResp& response) {
    // TO-DO 
}

void MsgClientHandler::GetMaxSeqsClient(const ::msg::GetMaxSeqsReq& request, ::msg::SeqsInfoResp& response) {
    // TO-DO 
}

void MsgClientHandler::GetHasReadSeqsClient(const ::msg::GetHasReadSeqsReq& request, ::msg::SeqsInfoResp& response) {
    // TO-DO 
}

void MsgClientHandler::GetMsgByConversationIDsClient(const ::msg::GetMsgByConversationIDsReq& request, ::msg::GetMsgByConversationIDsResp& response) {
    // TO-DO 
}

void MsgClientHandler::GetConversationMaxSeqClient(const ::msg::GetConversationMaxSeqReq& request, ::msg::GetConversationMaxSeqResp& response) {
    // TO-DO 
}

void MsgClientHandler::PullMessageBySeqsClient(const ::sdkws::PullMessageBySeqsReq& request, ::sdkws::PullMessageBySeqsResp& response) {
    // TO-DO 
}

void MsgClientHandler::GetSeqMessageClient(const ::msg::GetSeqMessageReq& request, ::msg::GetSeqMessageResp& response) {
    // TO-DO 
}

void MsgClientHandler::SearchMessageClient(const ::msg::SearchMessageReq& request, ::msg::SearchMessageResp& response) {
    // TO-DO 
}

void MsgClientHandler::SendMessagesClient(const ::sdkws::SendMessageReq& request, ::sdkws::SendMessageResp& response) {

    if(request.msgs_size() > 0){
        spdlog::info("SendMessages called, first msg, convID: {}, sendID: {}, clientMsgID: {}, msgCount: {}", 
            request.msgs(0).convid(), request.msgs(0).sendid(), request.msgs(0).clientmsgid(), request.msgs_size());
    }else{
        return;
    }

    ::grpc::ClientContext context;
    ::grpc::Status status = stub->SendMessages(&context, request, &response);
    if (!status.ok()) {
        spdlog::error("SendMessages failed, status: {}", status.error_message());
    }else{
        // 简单打印一下响应
        spdlog::info("SendMessages success, convID: {}, clientMsgID: {}", request.msgs(0).convid(), request.msgs(0).clientmsgid());
    }
}

void MsgClientHandler::SendSimpleMsgClient(const ::msg::SendSimpleMsgReq& request, ::msg::SendSimpleMsgResp& response) {
    // TO-DO 
}

void MsgClientHandler::SetUserConversationsMinSeqClient(const ::msg::SetUserConversationsMinSeqReq& request, ::msg::SetUserConversationsMinSeqResp& response) {
    // TO-DO 
}

void MsgClientHandler::ClearConversationsMsgClient(const ::msg::ClearConversationsMsgReq& request, ::msg::ClearConversationsMsgResp& response) {
    // TO-DO 
}

void MsgClientHandler::UserClearAllMsgClient(const ::msg::UserClearAllMsgReq& request, ::msg::UserClearAllMsgResp& response) {
    // TO-DO 
}

void MsgClientHandler::DeleteMsgsClient(const ::msg::DeleteMsgsReq& request, ::msg::DeleteMsgsResp& response) {
    // TO-DO 
}

void MsgClientHandler::DeleteMsgPhysicalBySeqClient(const ::msg::DeleteMsgPhysicalBySeqReq& request, ::msg::DeleteMsgPhysicalBySeqResp& response) {
    // TO-DO 
}

void MsgClientHandler::DeleteMsgPhysicalClient(const ::msg::DeleteMsgPhysicalReq& request, ::msg::DeleteMsgPhysicalResp& response) {
    // TO-DO 
}

void MsgClientHandler::SetSendMsgStatusClient(const ::msg::SetSendMsgStatusReq& request, ::msg::SetSendMsgStatusResp& response) {
    // TO-DO 
}

void MsgClientHandler::GetSendMsgStatusClient(const ::msg::GetSendMsgStatusReq& request, ::msg::GetSendMsgStatusResp& response) {
    // TO-DO 
}

void MsgClientHandler::RevokeMsgClient(const ::msg::RevokeMsgReq& request, ::msg::RevokeMsgResp& response) {
    // TO-DO 
}

void MsgClientHandler::MarkMsgsAsReadClient(const ::msg::MarkMsgsAsReadReq& request, ::msg::MarkMsgsAsReadResp& response) {
    // TO-DO 
}

void MsgClientHandler::MarkConversationAsReadClient(const ::msg::MarkConversationAsReadReq& request, ::msg::MarkConversationAsReadResp& response) {
    // TO-DO 
}

void MsgClientHandler::SetConversationHasReadSeqClient(const ::msg::SetConversationHasReadSeqReq& request, ::msg::SetConversationHasReadSeqResp& response) {
    // TO-DO 
}

void MsgClientHandler::GetConversationsHasReadAndMaxSeqClient(const ::msg::GetConversationsHasReadAndMaxSeqReq& request, ::msg::GetConversationsHasReadAndMaxSeqResp& response) {
    // TO-DO 
}

void MsgClientHandler::GetActiveUserClient(const ::msg::GetActiveUserReq& request, ::msg::GetActiveUserResp& response) {
    // TO-DO 
}

void MsgClientHandler::GetActiveGroupClient(const ::msg::GetActiveGroupReq& request, ::msg::GetActiveGroupResp& response) {
    // TO-DO 
}

void MsgClientHandler::GetServerTimeClient(const ::msg::GetServerTimeReq& request, ::msg::GetServerTimeResp& response) {
    // TO-DO 
}

void MsgClientHandler::ClearMsgClient(const ::msg::ClearMsgReq& request, ::msg::ClearMsgResp& response) {
    // TO-DO 
}

void MsgClientHandler::DestructMsgsClient(const ::msg::DestructMsgsReq& request, ::msg::DestructMsgsResp& response) {
    // TO-DO 
}

void MsgClientHandler::GetActiveConversationClient(const ::msg::GetActiveConversationReq& request, ::msg::GetActiveConversationResp& response) {
    // TO-DO 
}

void MsgClientHandler::SetUserConversationMaxSeqClient(const ::msg::SetUserConversationMaxSeqReq& request, ::msg::SetUserConversationMaxSeqResp& response) {
    // TO-DO 
}

void MsgClientHandler::SetUserConversationMinSeqClient(const ::msg::SetUserConversationMinSeqReq& request, ::msg::SetUserConversationMinSeqResp& response) {
    // TO-DO 
}

void MsgClientHandler::GetLastMessageSeqByTimeClient(const ::msg::GetLastMessageSeqByTimeReq& request, ::msg::GetLastMessageSeqByTimeResp& response) {
    // TO-DO 
}

void MsgClientHandler::GetLastMessageClient(const ::msg::GetLastMessageReq& request, ::msg::GetLastMessageResp& response) {
    // TO-DO 
}