#include "services/backbon_service/client.h"
#include <grpcpp/support/status.h>
#include <spdlog/spdlog.h>
#include <string>
#include <type_traits>
#include <vector>

backbon::CheckUserOnlineResp BackbonClient::CheckUserOnline() {

  backbon::CheckUserOnlineReq request;
  backbon::CheckUserOnlineResp reply;

  // Context for the client. It could be used to convey extra information to
  // the server and/or tweak certain RPC behaviors.
  grpc::ClientContext context;

  // The actual RPC.
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->CheckUserOnline(&context, &request, &reply,
                                  [&mu, &cv, &done, &status](grpc::Status s) {
                                    status = std::move(s);
                                    std::lock_guard<std::mutex> lock(mu);
                                    done = true;
                                    cv.notify_one();
                                  });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  // Act upon its status.
  if (!status.ok()) {
    spdlog::error("CheckUserOnline failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }

  return reply;
}

backbon::RegisterServiceResp BackbonClient::RegisterService(ServiceInfo service_info) {
  backbon::RegisterServiceReq request;
  backbon::RegisterServiceResp reply;

  request.set_service(service_info.service);
  for (const auto &ipport : service_info.ipports) {
    request.add_ipport(ipport);
  }
  for (const auto &method : service_info.methods) {
    request.add_methods(method);
  }

  // 测试用
  // request.set_service("backbon_service");
  // request.add_ipport("127.0.0.1:50001");
  // request.add_ipport("127.0.0.1:50002");
  // request.add_ipport("127.0.0.1:50003");
  // request.add_methods("CheckUserOnline");
  // request.add_methods("RegisterService");
  // request.add_methods("UnregisterService");


  // Context for the client. It could be used to convey extra information to
  // the server and/or tweak certain RPC behaviors.
  grpc::ClientContext context;

  // The actual RPC.
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->RegisterService(&context, &request, &reply,
                                  [&mu, &cv, &done, &status](grpc::Status s) {
                                    status = std::move(s);
                                    std::lock_guard<std::mutex> lock(mu);
                                    done = true;
                                    cv.notify_one();
                                  });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  // Act upon its status.
  if (!status.ok()) {
    spdlog::error("RegisterService failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }

  return reply;
}

backbon::UnregisterServiceResp BackbonClient::UnregisterService() {
  backbon::UnregisterServiceReq request;
  backbon::UnregisterServiceResp reply;

  // Context for the client. It could be used to convey extra information to
  // the server and/or tweak certain RPC behaviors.
  grpc::ClientContext context;

  // The actual RPC.
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->UnregisterService(&context, &request, &reply,
                                  [&mu, &cv, &done, &status](grpc::Status s) {
                                    status = std::move(s);
                                    std::lock_guard<std::mutex> lock(mu);
                                    done = true;
                                    cv.notify_one();
                                  });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  // Act upon its status.
  if (!status.ok()) {
    spdlog::error("UnregisterService failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }

  return reply;
}

backbon::GetServiceResp BackbonClient::GetServicesList() {
  backbon::GetServiceReq request;
  backbon::GetServiceResp reply;

  // Context for the client. It could be used to convey extra information to
  // the server and/or tweak certain RPC behaviors.
  grpc::ClientContext context;

  // The actual RPC.
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  grpc::Status status;
  stub_->async()->GetService(&context, &request, &reply,    
                                  [&mu, &cv, &done, &status](grpc::Status s) {
                                    status = std::move(s);
                                    std::lock_guard<std::mutex> lock(mu);
                                    done = true;
                                    cv.notify_one();
                                  });

  std::unique_lock<std::mutex> lock(mu);
  while (!done) {
    cv.wait(lock);
  }

  // Act upon its status.
  if (!status.ok()) {
    spdlog::error("GetService failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }

  return reply;
}
