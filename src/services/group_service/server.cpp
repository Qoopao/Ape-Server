#include "storage/mysqlconnector.h"
#include "gateway/ws_session_manager.h"
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_future.hpp>
#include "services/group_service/server.h"
#include "storage/mysqlhandler.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <chrono>
#include <cstdint>

GroupServiceImpl::GroupServiceImpl(const std::string &service_name,
                                   const std::string &listen_address)
    : BaseServiceServer<GroupServiceImpl>(service_name, listen_address) {}

GroupServiceImpl::~GroupServiceImpl() { Stop(); StopDelivery(); }

grpc::ServerUnaryReactor* GroupServiceImpl::Execute(grpc::CallbackServerContext* context,
    const groupchat::Command* request,groupchat::Reply* response) {
    auto* reactor=context->DefaultReactor();
    boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(0),
        [this,reactor,response,command=*request]()->boost::asio::awaitable<void> {
            try {
                *response=co_await MySQLHandler::ExecuteGroupCommand(command);
                const auto op=command.operation();
                if(response->errorcode()=="0" && op>=groupchat::Create && op<=groupchat::Dismiss &&
                    op!=groupchat::Search && op!=groupchat::Applications && op!=groupchat::Members)
                    co_await Notify(response->groupid());
                reactor->Finish(grpc::Status::OK);
            } catch(const std::exception& e) {
                spdlog::error("GroupService.Execute: {}",e.what());
                reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL,"Group service unavailable"));
            }
        },boost::asio::detached);
    return reactor;
}

boost::asio::awaitable<void> GroupServiceImpl::Notify(std::string id) {
    if(id.empty()) co_return;
    try {
        groupchat::Notice notice; 
        notice.set_groupid(id); 
        notice.set_refresh(true);
        for(auto account:co_await MySQLHandler::GroupNotificationAccounts(std::move(id))) {
            if(!WSSessionManager::instance().hasSession(account)) continue;
            sdkws::SdkWSResp frame; 
            frame.set_type(251); 
            frame.set_account(account); 
            frame.set_data(notice.SerializeAsString());
            WSSessionManager::instance().pushToUser(account,std::make_shared<std::string>(frame.SerializeAsString()));
        }
    } catch(const std::exception& e) { spdlog::warn("GroupService refresh notice: {}",e.what()); }
}

void GroupServiceImpl::StartDelivery() {
    if(m_deliveryWorker.valid()) return;
    m_deliveryStopping=false;
    m_deliveryWorker=boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(0),Deliver(),boost::asio::use_future);
}
void GroupServiceImpl::StopDelivery() {
    m_deliveryStopping=true;
    if(!m_deliveryWorker.valid()) return;
    try { m_deliveryWorker.get(); }
    catch(const std::exception& e) { spdlog::error("GroupService delivery shutdown: {}",e.what()); }
}
boost::asio::awaitable<void> GroupServiceImpl::Deliver() {
    using boost::asio::use_awaitable;
    boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
    while(!m_deliveryStopping.load()) {
        try {
            for(auto actor:WSSessionManager::instance().onlineAccounts()) {
                if(m_deliveryStopping.load()) break;
                for(const auto& id:co_await MySQLHandler::PendingGroupsForDelivery(actor)) {
                    // The handler keeps the membership lock until the synchronous push is queued.
                    co_await MySQLHandler::WithGroupDelivery(actor,id,[actor](const groupchat::Notice& notice) {
                        sdkws::SdkWSResp frame; frame.set_type(251); frame.set_account(actor); frame.set_data(notice.SerializeAsString());
                        WSSessionManager::instance().pushToUser(actor,std::make_shared<std::string>(frame.SerializeAsString()));
                    });
                }
            }
        } catch(const std::exception& e) { spdlog::warn("GroupService delivery retry: {}",e.what()); }
        timer.expires_after(std::chrono::seconds(2)); co_await timer.async_wait(use_awaitable);
    }
}
