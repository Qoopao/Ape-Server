#ifndef OFFLINE_MSG_HANDLER_H
#define OFFLINE_MSG_HANDLER_H

#include "messagequeue/kafkahandler.h"
#include <grpcpp/grpcpp.h>
#include "push.grpc.pb.h"
#include "sdkws.pb.h"
#include <spdlog/spdlog.h>
#include <memory>
#include <string>

// OfflineMsgHandler: 消费离线消息Kafka Topic，将消息持久化到 Redis（热数据）和 MongoDB（冷数据）
class OfflineMsgHandler : public KafkaHandler {
public:
    OfflineMsgHandler(std::shared_ptr<grpc::Channel> pushChannel);
    void handle(const std::string& topic, const std::string& msg) override;
private:
    std::unique_ptr<push::PushService::Stub> pushStub_;
};

#endif