#ifndef BACKBON_SERVICE_CLIENT_H
#define BACKBON_SERVICE_CLIENT_H

#include "backbon.grpc.pb.h"
#include "backbon.pb.h"
#include <grpcpp/channel.h>
#include <boost/asio/awaitable.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

class BackbonServiceImpl;

struct ServiceInfo {
    std::string service;
    std::vector<std::string> ipports;
    std::vector<std::string> methods;
};

class BackbonClient {
 public:
  BackbonClient(std::shared_ptr<grpc::Channel> channel)
      : stub_(backbon::BackbonService::NewStub(channel)) {}

  boost::asio::awaitable<backbon::CheckUserOnlineResp> CheckUserOnline();
  boost::asio::awaitable<backbon::RegisterServiceResp> RegisterService(ServiceInfo service_info);
  boost::asio::awaitable<backbon::UnregisterServiceResp> UnregisterService();
  boost::asio::awaitable<backbon::GetServiceResp> GetServicesList(const std::string &service_name);

 private:
  std::unique_ptr<backbon::BackbonService::Stub> stub_;
};

#endif