#include "services/conversation_service/server.h"
#include "storage/mysqlhandler.h"
#include "storage/mysqlconnector.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <algorithm>

ConversationServiceImpl::ConversationServiceImpl(const std::string& name, const std::string& address)
    : BaseServiceServer<ConversationServiceImpl>(name, address)
{
}

grpc::ServerUnaryReactor* ConversationServiceImpl::GetAllConversations(
    grpc::CallbackServerContext* context, const conversation::GetAllConversationsReq* request,
    conversation::GetAllConversationsResp* response)
{
    auto* reactor = context->DefaultReactor();
    boost::asio::co_spawn(MySQLConnector::instance().get_executor(),
        [reactor, response, owner = request->owneruserid()]() -> boost::asio::awaitable<void> {
            try {
                if (owner == 0) {
                    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Missing owner"));
                    co_return;
                }
                for (auto& item : co_await MySQLHandler::ListConversations(owner))
                    *response->add_conversations() = std::move(item);
                reactor->Finish(grpc::Status::OK);
            } catch (const std::exception& e) {
                spdlog::error("ConversationService.GetAllConversations: {}", e.what());
                reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, "Conversation list unavailable"));
            }
        }, boost::asio::detached);
    return reactor;
}

grpc::ServerUnaryReactor* ConversationServiceImpl::GetConversation(
    grpc::CallbackServerContext* context, const conversation::GetConversationReq* request,
    conversation::GetConversationResp* response)
{
    auto* reactor = context->DefaultReactor();
    boost::asio::co_spawn(MySQLConnector::instance().get_executor(),
        [reactor, response, owner = request->owneruserid(), id = request->conversationid()]()
            -> boost::asio::awaitable<void> {
            try {
                auto found = co_await MySQLHandler::FindConversation(owner, id);
                if (!found)
                    reactor->Finish(grpc::Status(grpc::StatusCode::NOT_FOUND, "Conversation not found"));
                else {
                    *response->mutable_conversation() = std::move(*found);
                    reactor->Finish(grpc::Status::OK);
                }
            } catch (const std::exception& e) {
                spdlog::error("ConversationService.GetConversation: {}", e.what());
                reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, "Conversation unavailable"));
            }
        }, boost::asio::detached);
    return reactor;
}

grpc::ServerUnaryReactor* ConversationServiceImpl::GetSortedConversationList(
    grpc::CallbackServerContext* context, const conversation::GetSortedConversationListReq* request,
    conversation::GetSortedConversationListResp* response)
{
    auto* reactor = context->DefaultReactor();
    auto query = *request;
    boost::asio::co_spawn(MySQLConnector::instance().get_executor(),
        [reactor, response, query = std::move(query)]() -> boost::asio::awaitable<void> {
            try {
                *response = co_await MySQLHandler::SortedConversations(std::move(query));
                reactor->Finish(grpc::Status::OK);
            } catch (const std::exception& e) {
                spdlog::error("ConversationService.GetSortedConversationList: {}", e.what());
                reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, "Conversation list unavailable"));
            }
        }, boost::asio::detached);
    return reactor;
}

grpc::ServerUnaryReactor* ConversationServiceImpl::MarkConversationRead(
    grpc::CallbackServerContext* context, const conversation::MarkConversationReadReq* request,
    conversation::MarkConversationReadResp*)
{
    auto* reactor = context->DefaultReactor();
    auto query = *request;
    boost::asio::co_spawn(MySQLConnector::instance().get_executor(),
        [reactor, query = std::move(query)]() -> boost::asio::awaitable<void> {
            try {
                if (!query.owneruserid() || query.conversationid().empty() || query.readseq() <= 0) {
                    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Invalid read position"));
                    co_return;
                }
                const bool updated = co_await MySQLHandler::MarkConversationRead(
                    query.owneruserid(), query.conversationid(), query.readseq());
                reactor->Finish(updated ? grpc::Status::OK :
                    grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "Invalid read position or missing conversation"));
            } catch (const std::exception& e) {
                spdlog::error("ConversationService.MarkConversationRead: {}", e.what());
                reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, "Read update unavailable"));
            }
        }, boost::asio::detached);
    return reactor;
}

grpc::ServerUnaryReactor* ConversationServiceImpl::UpdateConversationSettings(
    grpc::CallbackServerContext* context, const conversation::UpdateConversationSettingsReq* request,
    conversation::UpdateConversationSettingsResp*)
{
    auto* reactor = context->DefaultReactor();
    auto query = *request;
    boost::asio::co_spawn(MySQLConnector::instance().get_executor(),
        [reactor, query = std::move(query)]() -> boost::asio::awaitable<void> {
            try {
                if (!query.owneruserid() || query.conversationid().empty()) {
                    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Invalid conversation"));
                    co_return;
                }
                bool updated = co_await MySQLHandler::UpdateConversationSettings(query.owneruserid(),
                    query.conversationid(), query.has_ispinned(), query.ispinned(),
                    query.has_recvmsgopt(), query.recvmsgopt());
                reactor->Finish(updated ? grpc::Status::OK :
                    grpc::Status(grpc::StatusCode::NOT_FOUND, "Conversation not found"));
            } catch (const std::exception& e) {
                spdlog::error("ConversationService.UpdateConversationSettings: {}", e.what());
                reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, "Settings unavailable"));
            }
        }, boost::asio::detached);
    return reactor;
}

grpc::ServerUnaryReactor* ConversationServiceImpl::DeleteConversation(
    grpc::CallbackServerContext* context, const conversation::DeleteConversationReq* request,
    conversation::DeleteConversationResp*)
{
    auto* reactor = context->DefaultReactor();
    auto query = *request;
    boost::asio::co_spawn(MySQLConnector::instance().get_executor(),
        [reactor, query = std::move(query)]() -> boost::asio::awaitable<void> {
            try {
                if (!query.owneruserid() || query.conversationid().empty()) {
                    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Invalid conversation"));
                    co_return;
                }
                co_await MySQLHandler::DeleteConversation(query.owneruserid(), query.conversationid());
                reactor->Finish(grpc::Status::OK);
            } catch (const std::exception& e) {
                spdlog::error("ConversationService.DeleteConversation: {}", e.what());
                reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, "Conversation deletion unavailable"));
            }
        }, boost::asio::detached);
    return reactor;
}
