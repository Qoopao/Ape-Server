#ifndef FRIEND_SERVICE_CLIENT_H
#define FRIEND_SERVICE_CLIENT_H

#include "friend.grpc.pb.h"
#include <boost/asio/awaitable.hpp>
#include <memory>

class FriendClient
{
public:
    explicit FriendClient(std::shared_ptr<grpc::Channel> channel);
    boost::asio::awaitable<friendpb::Reply> Execute(const friendpb::Command& command);

private:
    std::unique_ptr<friendpb::FriendService::Stub> m_stub;
};

#endif
