#ifndef MESSAGE_PRODUCER_H
#define MESSAGE_PRODUCER_H

#include <string>

// 通用消息生产者接口：业务层只依赖此抽象，不感知具体 MQ 实现
class IMessageProducer {
public:
    virtual ~IMessageProducer() = default;

    virtual bool deliver(std::string &key, void *payload, size_t payloadSize) = 0;
};

#endif
