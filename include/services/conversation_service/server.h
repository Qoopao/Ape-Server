#ifndef CONVERSATION_SERVICE_SERVER_H
#define CONVERSATION_SERVICE_SERVER_H

#include "conversation.grpc.pb.h"
#include "services/base_service.h"

class ConversationServiceImpl final : public conversation::ConversationService::CallbackService,
                                      public BaseServiceServer<ConversationServiceImpl>
{
public:
    ConversationServiceImpl(const std::string& name, const std::string& address);
    grpc::ServerUnaryReactor* GetAllConversations(grpc::CallbackServerContext* context,
        const conversation::GetAllConversationsReq* request,
        conversation::GetAllConversationsResp* response) override;
    grpc::ServerUnaryReactor* GetConversation(grpc::CallbackServerContext* context,
        const conversation::GetConversationReq* request,
        conversation::GetConversationResp* response) override;
    grpc::ServerUnaryReactor* GetSortedConversationList(grpc::CallbackServerContext* context,
        const conversation::GetSortedConversationListReq* request,
        conversation::GetSortedConversationListResp* response) override;
    grpc::ServerUnaryReactor* MarkConversationRead(grpc::CallbackServerContext* context,
        const conversation::MarkConversationReadReq* request,
        conversation::MarkConversationReadResp* response) override;
    grpc::ServerUnaryReactor* UpdateConversationSettings(grpc::CallbackServerContext* context,
        const conversation::UpdateConversationSettingsReq* request,
        conversation::UpdateConversationSettingsResp* response) override;
    grpc::ServerUnaryReactor* DeleteConversation(grpc::CallbackServerContext* context,
        const conversation::DeleteConversationReq* request,
        conversation::DeleteConversationResp* response) override;
};

#endif
