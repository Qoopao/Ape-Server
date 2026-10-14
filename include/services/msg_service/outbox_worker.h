#ifndef OUTBOX_WORKER_H
#define OUTBOX_WORKER_H

#include <atomic>
#include <future>
#include <vector>
#include <boost/asio/awaitable.hpp>

class IMessageProducer;

class OutboxWorker
{
public:
    explicit OutboxWorker(IMessageProducer& producer);
    ~OutboxWorker();
    void Start();
    void Stop();

private:
    boost::asio::awaitable<void> Run();
    IMessageProducer& m_producer;
    std::atomic<bool> m_stopping{false};
    std::vector<std::future<void>> m_workers;
};

#endif
