#include "services/conversation_service/client.h"
#include "util/grpc_async_util.h"
#include <chrono>
#include <stdexcept>

ConversationClient::ConversationClient(std::shared_ptr<grpc::Channel> channel)
    : m_stub(conversation::ConversationService::NewStub(channel))
{
}

boost::asio::awaitable<conversation::GetAllConversationsResp> ConversationClient::List(uint64_t owner)
{
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(8));
    conversation::GetAllConversationsReq request;
    request.set_owneruserid(owner);
    conversation::GetAllConversationsResp response;
    auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
        m_stub->async()->GetAllConversations(&context, &request, &response,
            std::forward<decltype(handler)>(handler));
    });
    if (!status.ok())
        throw std::runtime_error(status.error_message());
    co_return response;
}

boost::asio::awaitable<void> ConversationClient::MarkRead(
    const conversation::MarkConversationReadReq& request)
{
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(8));
    conversation::MarkConversationReadResp response;
    auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
        m_stub->async()->MarkConversationRead(&context, &request, &response,
            std::forward<decltype(handler)>(handler));
    });
    if (!status.ok())
        throw std::runtime_error(status.error_message());
}

boost::asio::awaitable<void> ConversationClient::UpdateSettings(
    const conversation::UpdateConversationSettingsReq& request)
{
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(8));
    conversation::UpdateConversationSettingsResp response;
    auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
        m_stub->async()->UpdateConversationSettings(&context, &request, &response,
            std::forward<decltype(handler)>(handler));
    });
    if (!status.ok())
        throw std::runtime_error(status.error_message());
}

boost::asio::awaitable<void> ConversationClient::Delete(const conversation::DeleteConversationReq& request)
{
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(8));
    conversation::DeleteConversationResp response;
    auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
        m_stub->async()->DeleteConversation(&context, &request, &response,
            std::forward<decltype(handler)>(handler));
    });
    if (!status.ok())
        throw std::runtime_error(status.error_message());
}
