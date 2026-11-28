#ifndef OUTBOX_WORKER_H
#define OUTBOX_WORKER_H

#include "services/gateway_router.h"

#include <atomic>
#include <future>
#include <vector>
#include <boost/asio/awaitable.hpp>

class IMessageProducer;

class OutboxWorker
{
public:
    // provider 延迟获取 BackbonClient（注册异步完成，构造时可能还没有）
    OutboxWorker(IMessageProducer& producer, GatewayRouter::BackbonProvider backbon_provider);
    ~OutboxWorker();
    void Start();
    void Stop();

private:
    // pool：本worker绑定的MySQL连接池下标（也是它所在的io_context）
    boost::asio::awaitable<void> Run(int pool);
    // 后台清理：只删除已完成的 outbox 行，按 operation 分别保留（11.1）
    boost::asio::awaitable<void> RunCleanup();
    IMessageProducer& m_producer;
    GatewayRouter m_router;
    std::atomic<bool> m_stopping{false};
    std::vector<std::future<void>> m_workers;
    std::future<void> m_cleaner;
};

#endif
