#ifndef MESSAGE_HANDLER_H
#define MESSAGE_HANDLER_H

#include <string>
#include <boost/asio/awaitable.hpp>

// 通用消息处理器接口：业务逻辑实现此接口来处理消费到的消息
class MessageHandler {
public:
    virtual boost::asio::awaitable<void> handle(const std::string topic, const std::string msg) = 0;
    virtual ~MessageHandler() = default;
};

#endif
