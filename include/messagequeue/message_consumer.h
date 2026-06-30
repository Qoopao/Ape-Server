#ifndef MESSAGE_CONSUMER_H
#define MESSAGE_CONSUMER_H

#include <memory>

class MessageHandler;
class IOC_Pool;

// 通用消息消费者接口
class IMessageConsumer {
public:
    virtual ~IMessageConsumer() = default;

    virtual void setHandler(std::shared_ptr<MessageHandler> handler) = 0;
    virtual void setIOCPool(IOC_Pool *pool) = 0;
    virtual void start() = 0;
    virtual void shutdown() = 0;
    virtual void join() = 0;
};

#endif
