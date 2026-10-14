#ifndef GROUP_SERVICE_SERVER_H
#define GROUP_SERVICE_SERVER_H

#include "groupchat.grpc.pb.h"
#include "groupchat.pb.h"
#include "services/base_service.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/status.h>
#include <spdlog/spdlog.h>
#include <string>
#include <atomic>
#include <future>

class GroupServiceImpl final : public groupchat::GroupService::CallbackService,
                               public BaseServiceServer<GroupServiceImpl> {
public:
  GroupServiceImpl(const std::string &service_name,
                   const std::string &listen_address);
  ~GroupServiceImpl() override;
  ::grpc::ServerUnaryReactor* Execute(::grpc::CallbackServerContext* context,
      const groupchat::Command* request,groupchat::Reply* response) override;
  void StartDelivery();
  void StopDelivery();

private:
  boost::asio::awaitable<void> Deliver();
  boost::asio::awaitable<void> Notify(std::string groupID);
  std::atomic<bool> m_deliveryStopping{false};
  std::future<void> m_deliveryWorker;
};

#endif
