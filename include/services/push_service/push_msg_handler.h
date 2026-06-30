#ifndef PUSH_MSG_HANDLER_H
#define PUSH_MSG_HANDLER_H

#include "messagequeue/message_handler.h"
#include "push.grpc.pb.h"
#include "push.pb.h"
#include <grpcpp/grpcpp.h>
#include <boost/asio/awaitable.hpp>
#include <memory>
#include <string>

// PushHandler: 消费消息队列中的消息并触发推送
// 从消息队列接收消息，解析后调用 push gRPC 服务进行离线推送
class PushHandler : public MessageHandler {
public:
    PushHandler(std::shared_ptr<grpc::Channel> pushChannel);
    boost::asio::awaitable<void> handle(const std::string topic, const std::string msgID) override;

private:
    std::unique_ptr<push::PushService::Stub> pushStub_;
};

#endif
