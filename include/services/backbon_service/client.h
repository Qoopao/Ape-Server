#ifndef BACKBON_SERVICE_CLIENT_H
#define BACKBON_SERVICE_CLIENT_H

#include "backbon.grpc.pb.h"
#include "backbon.pb.h"
#include <grpcpp/channel.h>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>

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

  backbon::CheckUserOnlineResp CheckUserOnline();
  backbon::RegisterServiceResp RegisterService(ServiceInfo service_info);
  backbon::UnregisterServiceResp UnregisterService();
  backbon::GetServiceResp GetServicesList();

 private:
  std::unique_ptr<backbon::BackbonService::Stub> stub_;

};

#endif
