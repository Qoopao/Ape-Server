#include "services/msg_service/outbox_worker.h"
#include "storage/mysqlhandler.h"
#include "storage/mysqlconnector.h"
#include "gateway/ws_session_manager.h"
#include "sdkws.pb.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <google/protobuf/util/json_util.h>
#include <spdlog/spdlog.h>

namespace {

void notifySender(const MySQLHandler::OutboxTask& task)
{
    if (task.operation != 1 || (task.stage == 0 && task.state != 3) || (task.stage == 1 && task.state == 3))
        return;
    sdkws::MsgData message;
    if (!google::protobuf::util::JsonStringToMessage(task.payload, &message).ok())
        return;
    sdkws::SendMessageResp result;
    auto* info = result.add_infos();
    *info->mutable_msg() = message;
    info->set_errorcode(task.state == 3 ? "NOT_FRIEND" : "0");
    if (task.state == 3)
        info->set_errormsg("Message cancelled because the friend relationship was removed");
    sdkws::SdkWSResp response;
    response.set_type(101);
    response.set_account(task.actor);
    response.set_requestid(message.clientmsgid());
    response.set_errorcode("0");
    response.set_data(result.SerializeAsString());
    WSSessionManager::instance().pushToUser(task.actor,
        std::make_shared<std::string>(response.SerializeAsString()));
}

}

OutboxWorker::OutboxWorker(IMessageProducer& producer)
    : m_producer(producer)
{
}

OutboxWorker::~OutboxWorker()
{
    Stop();
}

void OutboxWorker::Start()
{
    if (!m_workers.empty())
        return;
    m_stopping = false;
    for (int i = 0; i < 2; ++i)
        m_workers.push_back(boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(0),
            Run(), boost::asio::use_future));
}

void OutboxWorker::Stop()
{
    m_stopping = true;
    for (auto& worker : m_workers) {
        try {
            worker.get();
        } catch (const std::exception& e) {
            spdlog::error("Outbox worker shutdown: {}", e.what());
        }
    }
    m_workers.clear();
}

boost::asio::awaitable<void> OutboxWorker::Run()
{
    boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
    while (!m_stopping.load()) {
        std::optional<MySQLHandler::OutboxTask> task;
        bool retry = false;
        std::string error;
        try {
            // 取出任务
            task = co_await MySQLHandler::ClaimOutboxTask();
            if (task)
                co_await MySQLHandler::ProcessOutboxTask(*task, m_producer);
        } catch (const std::exception& e) {
            retry = true;
            error = e.what();
            spdlog::error("Outbox task={} attempt={} failed: {}", task ? task->id : 0,
                task ? task->attempts : 0, error);
        }
        if (task) {
            try {
                if (retry)
                    co_await MySQLHandler::RetryOutboxTask(*task, error);
                notifySender(co_await MySQLHandler::GetOutboxTask(task->id));
            } catch (const std::exception& e) {
                spdlog::error("Outbox task={} recovery deferred to lease expiry: {}", task->id, e.what());
            }
        }
        if (!task || retry) {
            timer.expires_after(std::chrono::milliseconds(retry ? 1000 : 250));
            co_await timer.async_wait(boost::asio::use_awaitable);
        }
    }
}
