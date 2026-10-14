#ifndef GROUP_SERVICE_CLIENT_H
#define GROUP_SERVICE_CLIENT_H

#include "groupchat.grpc.pb.h"
#include "groupchat.pb.h"
#include <grpcpp/channel.h>
#include <boost/asio/awaitable.hpp>
#include <memory>
#include <string>
#include <vector>

class GroupClient {
public:
  GroupClient(std::shared_ptr<grpc::Channel> channel);
  boost::asio::awaitable<groupchat::Reply> Execute(const groupchat::Command& request);

private:
  std::unique_ptr<groupchat::GroupService::Stub> stub_;
};

#endif
