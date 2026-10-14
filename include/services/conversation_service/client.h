#ifndef CONVERSATION_SERVICE_CLIENT_H
#define CONVERSATION_SERVICE_CLIENT_H

#include "conversation.grpc.pb.h"
#include <boost/asio/awaitable.hpp>
#include <memory>

class ConversationClient
{
public:
    explicit ConversationClient(std::shared_ptr<grpc::Channel> channel);
    boost::asio::awaitable<conversation::GetAllConversationsResp> List(uint64_t owner);
    boost::asio::awaitable<void> MarkRead(const conversation::MarkConversationReadReq& request);
    boost::asio::awaitable<void> UpdateSettings(const conversation::UpdateConversationSettingsReq& request);
    boost::asio::awaitable<void> Delete(const conversation::DeleteConversationReq& request);

private:
    std::unique_ptr<conversation::ConversationService::Stub> m_stub;
};

#endif
