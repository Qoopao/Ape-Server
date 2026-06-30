#ifndef GATEWAY_PUSH_SERVICE_CLIENT_H
#define GATEWAY_PUSH_SERVICE_CLIENT_H

#include "gateway_push.grpc.pb.h"
#include "gateway_push.pb.h"
#include <grpcpp/channel.h>
#include <boost/asio/awaitable.hpp>
#include <memory>
#include <string>

class GatewayPushClient {
public:
    GatewayPushClient(std::shared_ptr<grpc::Channel> channel);

    boost::asio::awaitable<bool> PushToUser(const std::string &userID,
                                            const std::string &msgDataBin,
                                            const std::string &conversationID);

private:
    std::unique_ptr<gateway_push::GatewayPushService::Stub> stub_;
};

#endif