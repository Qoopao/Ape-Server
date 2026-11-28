#include "services/outbox_metrics.h"

#include "om/obs_metrics.h"
#include "storage/mysqlconnector.h"
#include "storage/mysqlhandler.h"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <mutex>

#include <spdlog/spdlog.h>

namespace ape {
namespace obs {

namespace {

boost::asio::awaitable<void> RunPoller()
{
    boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
    for (;;)
    {
        try
        {
            const auto rows = co_await MySQLHandler::CollectOutboxBacklog(0);
            for (const auto &row : rows)
                ape::obs::SetOutboxBacklog(row.operation, row.pending, row.oldest_age_seconds);
        }
        catch (const std::exception &e)
        {
            // 指标采集失败不能影响 outbox 主链路。
            spdlog::debug("outbox metrics poll failed: {}", e.what());
        }
        timer.expires_after(std::chrono::seconds(30));
        co_await timer.async_wait(boost::asio::use_awaitable);
    }
}

} // namespace

void StartOutboxMetricsPoller(int pool)
{
    static std::once_flag once;
    std::call_once(once, [pool]() {
        boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(pool),
                              RunPoller(), boost::asio::detached);
    });
}

} // namespace obs
} // namespace ape
