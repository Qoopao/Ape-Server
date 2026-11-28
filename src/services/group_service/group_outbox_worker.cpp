#include "services/group_service/group_outbox_worker.h"
#include "messagequeue/message_producer.h"
#include "om/obs_metrics.h"
#include "services/outbox_metrics.h"
#include "storage/mysqlconnector.h"
#include "storage/mysqlhandler.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <spdlog/spdlog.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

namespace {

int64_t EnvInt(const char *name, int64_t fallback, int64_t minimum, int64_t maximum) {
  const char *value = std::getenv(name);
  if (!value || !*value)
    return fallback;
  try {
    return std::clamp<int64_t>(std::stoll(value), minimum, maximum);
  } catch (const std::exception &) {
    spdlog::warn("GroupOutbox: invalid {}={}, using {}", name, value, fallback);
    return fallback;
  }
}

int64_t NowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

GroupOutboxWorker::GroupOutboxWorker(IMessageProducer &producer) : producer_(producer) {}

GroupOutboxWorker::~GroupOutboxWorker() { Stop(); }

void GroupOutboxWorker::Start() {
  if (!workers_.empty())
    return;
  stopping_.store(false);
  const int pool_count = std::max(1, MySQLConnector::instance().pool_size());
  int worker_count = pool_count;
  if (const char *value = std::getenv("GROUP_OUTBOX_WORKERS")) {
    try {
      worker_count = std::stoi(value);
    } catch (const std::exception &) {
      worker_count = pool_count;
    }
  }
  // 0 是合法值：测试用 GROUP_OUTBOX_WORKERS=0 暂停投递，观察 staging/outbox stage=0。
  worker_count = std::clamp(worker_count, 0, pool_count);
  if (worker_count == 0)
    spdlog::warn("GroupOutboxWorker: GROUP_OUTBOX_WORKERS=0, no publish workers started");
  ape::obs::StartOutboxMetricsPoller(0);

  for (int i = 0; i < worker_count; ++i) {
    workers_.push_back(boost::asio::co_spawn(
        MySQLConnector::instance().get_pool_executor(i), Run(i), boost::asio::use_future));
    spdlog::info("GroupOutboxWorker[{}] started on MySQL pool {}", i, i);
  }
  maintenance_ = boost::asio::co_spawn(
      MySQLConnector::instance().get_pool_executor(0), RunMaintenance(),
      boost::asio::use_future);
}

void GroupOutboxWorker::Stop() {
  stopping_.store(true);
  for (auto &worker : workers_) {
    try {
      worker.get();
    } catch (const std::exception &e) {
      spdlog::error("Group outbox worker shutdown: {}", e.what());
    }
  }
  workers_.clear();
  if (maintenance_.valid()) {
    try {
      maintenance_.get();
    } catch (const std::exception &e) {
      spdlog::error("Group outbox maintenance shutdown: {}", e.what());
    }
  }
}

boost::asio::awaitable<void> GroupOutboxWorker::Run(int pool) {
  const auto retryMs = EnvInt("GROUP_PUBLISH_RETRY_MS", 5000, 100, 3600000);
  boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
  while (!stopping_.load()) {
    std::optional<MySQLHandler::OutboxTask> task;
    bool retry = false;
    std::string error;
    try {
      task = co_await MySQLHandler::ClaimOutboxTask({4}, pool);
      if (task)
      {
        co_await MySQLHandler::ProcessGroupOutboxTask(*task, producer_, pool);
        ape::obs::RecordOutboxAttempt(4, "ok");
      }
    } catch (const std::exception &e) {
      retry = true;
      error = e.what();
      ape::obs::RecordOutboxAttempt(4, "error");
      spdlog::error("GroupOutbox[pool={}] task={} attempt={} failed: {}", pool,
          task ? task->id : 0, task ? task->attempts : 0, error);
    }
    if (task && retry) {
      try {
        // 群发布重试基线由 GROUP_PUBLISH_RETRY_MS 控制（默认 5s）。
        co_await MySQLHandler::RetryOutboxTask(*task, error, pool, retryMs);
      } catch (const std::exception &e) {
        spdlog::error("GroupOutbox task={} retry deferred to lease expiry: {}", task->id, e.what());
      }
    }
    if (!task || retry) {
      timer.expires_after(std::chrono::milliseconds(retry ? 1000 : 250));
      co_await timer.async_wait(boost::asio::use_awaitable);
    }
  }
}

boost::asio::awaitable<void> GroupOutboxWorker::RunMaintenance() {
  const auto outboxDays = EnvInt("GROUP_OUTBOX_RETENTION_DAYS", 7, 0, 3650);
  const auto stagingHours = EnvInt("GROUP_STAGING_RETENTION_HOURS", 24, 1, 720);
  const auto requestDays = EnvInt("GROUP_REQUEST_RETENTION_DAYS", 7, 0, 3650);
  constexpr int kBatch = 1000;
  constexpr int kMaxBatches = 10;
  boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
  while (!stopping_.load()) {
    try {
      const auto now = NowMillis();
      const auto removed = co_await MySQLHandler::PurgeCompletedOutbox(
          4, now - outboxDays * 24 * 60 * 60 * 1000, kBatch, kMaxBatches);
      if (removed)
        spdlog::info("GroupOutbox: removed {} completed op=4 rows older than {}d",
                     removed, outboxDays);
      for (int batch = 0; batch < kMaxBatches && !stopping_.load(); ++batch) {
        const auto staging = co_await MySQLHandler::PurgeOrphanGroupStaging(
            now - stagingHours * 60 * 60 * 1000, kBatch);
        if (staging < static_cast<uint64_t>(kBatch))
          break;
      }
      const auto requests = co_await MySQLHandler::PurgeGroupRequests(
          now - requestDays * 24 * 60 * 60 * 1000, kBatch);
      if (requests)
        spdlog::info("GroupOutbox: removed {} idempotency requests older than {}d",
                     requests, requestDays);
    } catch (const std::exception &e) {
      spdlog::warn("GroupOutbox maintenance failed: {}", e.what());
    }
    // 分片睡眠，Stop() 不被完整清理间隔阻塞。
    const auto next = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    while (!stopping_.load() && std::chrono::steady_clock::now() < next) {
      timer.expires_after(std::chrono::milliseconds(250));
      co_await timer.async_wait(boost::asio::use_awaitable);
    }
  }
}
