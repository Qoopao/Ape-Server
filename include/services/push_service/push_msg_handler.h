#ifndef PUSH_MSG_HANDLER_H
#define PUSH_MSG_HANDLER_H

#include "messagequeue/kafkahandler.h"
#include "push.grpc.pb.h"
#include "push.pb.h"
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>

// PushHandler: 消费Kafka消息并触发推送
// 从Kafka接收消息，解析后调用push gRPC服务进行离线推送
class PushHandler : public KafkaHandler {
public:
    PushHandler(std::shared_ptr<grpc::Channel> pushChannel);
    void handle(const std::string& topic, const std::string& msg) override;

private:
    std::unique_ptr<push::PushService::Stub> pushStub_;
};

#endif