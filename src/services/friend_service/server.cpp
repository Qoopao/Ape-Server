#include "services/friend_service/server.h"
#include "storage/mysqlhandler.h"
#include "storage/mysqlconnector.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>

FriendServiceImpl::FriendServiceImpl(const std::string& name, const std::string& address)
    : BaseServiceServer<FriendServiceImpl>(name, address)
{
}

grpc::ServerUnaryReactor* FriendServiceImpl::Execute(grpc::CallbackServerContext* context,
    const friendpb::Command* request, friendpb::Reply* response)
{
    auto* reactor = context->DefaultReactor();
    boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(0),
        [reactor, response, command = *request]() -> boost::asio::awaitable<void> {
            try {
                *response = co_await MySQLHandler::ExecuteFriendCommand(command);
                reactor->Finish(grpc::Status::OK);
            } catch (const std::exception& e) {
                spdlog::error("FriendService: {}", e.what());
                reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL, "Friend service unavailable"));
            }
        }, boost::asio::detached);
    return reactor;
}
