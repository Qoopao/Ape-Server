#include "services/conversation_service/server.h"
#include "storage/mysqlhandler.h"
#include "storage/mysqlconnector.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <stdexcept>

namespace {

void SetResult(conversation::OperationResult* result, const std::string& code,
               const std::string& message)
{
    result->set_errorcode(code);
    result->set_errormessage(message);
}

} // namespace

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
                    SetResult(response->mutable_result(), "INVALID_PARAM", "Missing owner");
                    reactor->Finish(grpc::Status::OK);
                    co_return;
                }
                for (auto& item : co_await MySQLHandler::ListConversations(owner))
                    *response->add_conversations() = std::move(item);
                SetResult(response->mutable_result(), "0", "");
                reactor->Finish(grpc::Status::OK);
            } catch (const std::exception& e) {
                spdlog::error("ConversationService.GetAllConversations: {}", e.what());
                // 非预期 DB 错误仍走 gRPC status，由网关映射为 CONVERSATION_ERROR。
                reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, "Conversation list unavailable"));
            }
        }, boost::asio::detached);
    return reactor;
}

grpc::ServerUnaryReactor* ConversationServiceImpl::MarkConversationRead(
    grpc::CallbackServerContext* context, const conversation::MarkConversationReadReq* request,
    conversation::MarkConversationReadResp* response)
{
    auto* reactor = context->DefaultReactor();
    auto query = *request;
    boost::asio::co_spawn(MySQLConnector::instance().get_executor(),
        [reactor, response, query = std::move(query)]() -> boost::asio::awaitable<void> {
            try {
                if (!query.owneruserid() || query.conversationid().empty() || query.readseq() <= 0) {
                    SetResult(response->mutable_result(), "INVALID_PARAM", "Invalid read position");
                    reactor->Finish(grpc::Status::OK);
                    co_return;
                }
                const bool updated = co_await MySQLHandler::MarkConversationRead(
                    query.owneruserid(), query.conversationid(), query.readseq());
                if (!updated) {
                    SetResult(response->mutable_result(), "INVALID_POSITION",
                              "Invalid read position or missing conversation");
                } else {
                    SetResult(response->mutable_result(), "0", "");
                }
                reactor->Finish(grpc::Status::OK);
            } catch (const std::exception& e) {
                spdlog::error("ConversationService.MarkConversationRead: {}", e.what());
                reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, "Read update unavailable"));
            }
        }, boost::asio::detached);
    return reactor;
}

grpc::ServerUnaryReactor* ConversationServiceImpl::UpdateConversationSettings(
    grpc::CallbackServerContext* context, const conversation::UpdateConversationSettingsReq* request,
    conversation::UpdateConversationSettingsResp* response)
{
    auto* reactor = context->DefaultReactor();
    auto query = *request;
    boost::asio::co_spawn(MySQLConnector::instance().get_executor(),
        [reactor, response, query = std::move(query)]() -> boost::asio::awaitable<void> {
            try {
                if (!query.owneruserid() || query.conversationid().empty()) {
                    SetResult(response->mutable_result(), "INVALID_PARAM", "Invalid conversation");
                    reactor->Finish(grpc::Status::OK);
                    co_return;
                }
                bool updated = false;
                try {
                    updated = co_await MySQLHandler::UpdateConversationSettings(query.owneruserid(),
                        query.conversationid(), query.has_ispinned(), query.ispinned(),
                        query.has_recvmsgopt(), query.recvmsgopt());
                } catch (const std::invalid_argument& e) {
                    // 两个字段都没传，或 recvMsgOpt 越界。
                    SetResult(response->mutable_result(), "INVALID_PARAM", e.what());
                    reactor->Finish(grpc::Status::OK);
                    co_return;
                }
                if (!updated) {
                    SetResult(response->mutable_result(), "NOT_FOUND", "Conversation not found");
                } else {
                    SetResult(response->mutable_result(), "0", "");
                }
                reactor->Finish(grpc::Status::OK);
            } catch (const std::exception& e) {
                spdlog::error("ConversationService.UpdateConversationSettings: {}", e.what());
                reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, "Settings unavailable"));
            }
        }, boost::asio::detached);
    return reactor;
}

grpc::ServerUnaryReactor* ConversationServiceImpl::DeleteConversation(
    grpc::CallbackServerContext* context, const conversation::DeleteConversationReq* request,
    conversation::DeleteConversationResp* response)
{
    auto* reactor = context->DefaultReactor();
    auto query = *request;
    boost::asio::co_spawn(MySQLConnector::instance().get_executor(),
        [reactor, response, query = std::move(query)]() -> boost::asio::awaitable<void> {
            try {
                if (!query.owneruserid() || query.conversationid().empty()) {
                    SetResult(response->mutable_result(), "INVALID_PARAM", "Invalid conversation");
                    reactor->Finish(grpc::Status::OK);
                    co_return;
                }
                co_await MySQLHandler::DeleteConversation(query.owneruserid(), query.conversationid());
                SetResult(response->mutable_result(), "0", "");
                reactor->Finish(grpc::Status::OK);
            } catch (const std::exception& e) {
                spdlog::error("ConversationService.DeleteConversation: {}", e.what());
                reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, "Conversation deletion unavailable"));
            }
        }, boost::asio::detached);
    return reactor;
}
