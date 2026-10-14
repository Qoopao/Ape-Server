#ifndef MESSAGE_PRODUCER_H
#define MESSAGE_PRODUCER_H

#include <string>
#include <boost/asio/awaitable.hpp>

// 通用消息生产者接口：业务层只依赖此抽象，不感知具体 MQ 实现
class IMessageProducer {
public:
    virtual ~IMessageProducer() = default;

    virtual bool deliver(std::string &key, void *payload, size_t payloadSize) = 0;
    virtual boost::asio::awaitable<bool> deliverConfirmed(std::string key, std::string payload) = 0;
};

#endif
