#include "services/msg_service/outbox_worker.h"
#include "om/obs_log.h"
#include "om/obs_metrics.h"
#include "services/outbox_metrics.h"
#include "storage/mysqlhandler.h"
#include "storage/mysqlconnector.h"
#include "sdkws.pb.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <algorithm>
#include <cstdlib>
#include <array>
#include <chrono>
#include <utility>

#include <google/protobuf/util/json_util.h>
#include <spdlog/spdlog.h>

namespace {

std::string GatewayFallbackAddress() {
    const char *value = std::getenv("GATEWAY_PUSH_ADDR");
    return value ? std::string(value) : std::string("localhost:50055");
}

// 延迟回执走路由推送：非网关进程不再直接访问 WSSessionManager（C-2/INV-5）
boost::asio::awaitable<void> notifySender(const MySQLHandler::OutboxTask& task,
                                          GatewayRouter& router)
{
    if (task.operation != 1 || (task.stage == 0 && task.state != 3) || (task.stage == 1 && task.state == 3))
        co_return;
    sdkws::MsgData message;
    if (!google::protobuf::util::JsonStringToMessage(task.payload, &message).ok())
        co_return;
    sdkws::SendMessageResp result;
    auto* info = result.add_infos();
    *info->mutable_msg() = message;
    info->set_errorcode(task.state == 3 ? "NOT_FRIEND" : "0");
    if (task.state == 3)
        info->set_errormsg("Message cancelled because the friend relationship was removed");
    const auto outcome = co_await router.PushFrame(
        task.actor, 101, result.SerializeAsString(), message.clientmsgid(), "0");
    if (outcome == PushResult::Unknown) {
        // 没问到网关：记日志，下个 tick/下次任务重试，不确认任何状态
        spdlog::warn("OutboxWorker: delayed receipt unknown for actor={}, task={}",
                     task.actor, task.id);
    }
}


// 已完成 outbox 任务的保留策略（docs/ack 11.1）：未完成任务永不删除。
struct OutboxRetention {
    int64_t days = 7;          // op=1 发送任务
    int64_t friendDays = 7;    // op=2 好友历史清理
    int64_t ackDays = 1;       // op=3 ACK 清理
    int batchLimit = 1000;
    int maxBatches = 10;
    int intervalMs = 600000;
};

int64_t EnvInt64(const char *name, int64_t fallback, int64_t minimum, int64_t maximum)
{
    const char *value = std::getenv(name);
    if (!value || !*value)
        return fallback;
    try {
        return std::clamp<int64_t>(std::stoll(value), minimum, maximum);
    } catch (const std::exception &) {
        spdlog::warn("OutboxCleanup: invalid {}={}, using {}", name, value, fallback);
        return fallback;
    }
}

OutboxRetention LoadRetention()
{
    OutboxRetention policy;
    policy.days = EnvInt64("OUTBOX_RETENTION_DAYS", policy.days, 0, 3650);
    policy.friendDays = EnvInt64("OUTBOX_FRIEND_RETENTION_DAYS", policy.friendDays, 0, 3650);
    policy.ackDays = EnvInt64("OUTBOX_ACK_RETENTION_DAYS", policy.ackDays, 0, 3650);
    policy.batchLimit = static_cast<int>(EnvInt64("OUTBOX_CLEANUP_BATCH", policy.batchLimit, 1, 5000));
    policy.maxBatches = static_cast<int>(EnvInt64("OUTBOX_CLEANUP_MAX_BATCHES", policy.maxBatches, 1, 100));
    policy.intervalMs = static_cast<int>(EnvInt64("OUTBOX_CLEANUP_INTERVAL_MS", policy.intervalMs, 200, 3600000));
    return policy;
}

int64_t NowMillis()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

OutboxWorker::OutboxWorker(IMessageProducer& producer,
                           GatewayRouter::BackbonProvider backbon_provider)
    : m_producer(producer),
      m_router(std::move(backbon_provider), GatewayFallbackAddress())
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

    // worker i ↔ MySQL pool i ↔ io_context i：每个 worker 独占一个连接池和线程，
    // 避免所有 worker 挤在 pool 0
    const int pool_count = std::max(1, MySQLConnector::instance().pool_size());
    int worker_count = pool_count;
    if (const char *value = std::getenv("OUTBOX_WORKERS")) {
        try {
            worker_count = std::stoi(value);
        } catch (const std::exception &) {
            spdlog::warn("OutboxWorker: invalid OUTBOX_WORKERS={}, using {}", value, pool_count);
            worker_count = pool_count;
        }
    }
    worker_count = std::clamp(worker_count, 1, pool_count);

    for (int i = 0; i < worker_count; ++i) {
        m_workers.push_back(boost::asio::co_spawn(
            MySQLConnector::instance().get_pool_executor(i),
            Run(i), boost::asio::use_future));
        spdlog::info("OutboxWorker[{}] started on MySQL pool {}", i, i);
    }

    ape::obs::StartOutboxMetricsPoller(0);

    // 清理线程：每个 MsgService 实例一个，按保留期删除已完成任务（11.1）
    m_cleaner = boost::asio::co_spawn(
        MySQLConnector::instance().get_pool_executor(0),
        RunCleanup(), boost::asio::use_future);
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
    if (m_cleaner.valid()) {
        try {
            m_cleaner.get();
        } catch (const std::exception &e) {
            spdlog::error("Outbox cleanup shutdown: {}", e.what());
        }
    }
}

boost::asio::awaitable<void> OutboxWorker::Run(int pool)
{
    boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
    while (!m_stopping.load()) {
        std::optional<MySQLHandler::OutboxTask> task;
        bool retry = false;
        std::string error;
        try {
            // 取出任务
            task = co_await MySQLHandler::ClaimOutboxTask({1, 2, 3}, pool);
            if (task)
            {
                co_await MySQLHandler::ProcessOutboxTask(*task, m_producer, pool);
                ape::obs::RecordOutboxAttempt(task->operation, "ok");
            }
        } catch (const std::exception& e) {
            retry = true;
            error = e.what();
            ape::obs::RecordOutboxAttempt(task ? task->operation : 0, "error");
            spdlog::error("Outbox worker[pool={}] task={} attempt={} failed: {}", pool,
                task ? task->id : 0, task ? task->attempts : 0, error);
        }
        if (task) {
            try {
                if (retry)
                    co_await MySQLHandler::RetryOutboxTask(*task, error, pool);
                co_await notifySender(
                    co_await MySQLHandler::GetOutboxTask(task->id, pool), m_router);
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

// 后台清理：只删 state IN (2,3) 且超过保留期的行；state 0/1（待执行/租约中）永不删除。
// 分片睡眠（250ms）保证 Stop() 不会被一个完整的清理间隔阻塞。
boost::asio::awaitable<void> OutboxWorker::RunCleanup()
{
    const auto policy = LoadRetention();
    spdlog::info("OutboxCleanup started: op1={}d op2={}d op3={}d batch={} maxBatches={} interval={}ms",
        policy.days, policy.friendDays, policy.ackDays, policy.batchLimit, policy.maxBatches,
        policy.intervalMs);

    const std::array<std::pair<int, int64_t>, 3> retention{{
        {1, policy.days}, {2, policy.friendDays}, {3, policy.ackDays}}};
    boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
    const auto interval = std::chrono::milliseconds(policy.intervalMs);
    while (!m_stopping.load()) {
        for (const auto &[operation, days] : retention) {
            try {
                const auto cutoff = NowMillis() - days * 24 * 60 * 60 * 1000;
                const auto deleted = co_await MySQLHandler::PurgeCompletedOutbox(
                    operation, cutoff, policy.batchLimit, policy.maxBatches);
                if (deleted)
                    spdlog::info("OutboxCleanup: removed {} completed op={} rows older than {}d",
                                 deleted, operation, days);
            } catch (const std::exception &e) {
                // 清理失败不影响投递：行还在，下一轮重试
                spdlog::warn("OutboxCleanup: op={} failed: {}", operation, e.what());
            }
        }
        const auto next = std::chrono::steady_clock::now() + interval;
        while (!m_stopping.load() && std::chrono::steady_clock::now() < next) {
            timer.expires_after(std::chrono::milliseconds(250));
            co_await timer.async_wait(boost::asio::use_awaitable);
        }
    }
}
