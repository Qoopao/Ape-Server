#include "services/backbon_service/client.h"
#include "util/grpc_async_util.h"

#include <chrono>
#include <grpcpp/support/status.h>
#include <spdlog/spdlog.h>

namespace {
// 注册/发现调用必须有截止时间：否则 etcd/Backbon 挂起时，注册重试与
// 看门狗线程会永久阻塞，连 Stop() 都退不出来。
void SetDeadline(grpc::ClientContext &context, int seconds) {
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(seconds));
}
} // namespace

boost::asio::awaitable<backbon::CheckUserOnlineResp> BackbonClient::CheckUserOnline() {
  backbon::CheckUserOnlineReq request;
  backbon::CheckUserOnlineResp reply;
  grpc::ClientContext context;
  SetDeadline(context, 3);

  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->CheckUserOnline(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });

  if (!status.ok()) {
    spdlog::error("CheckUserOnline failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }

  co_return reply;
}

boost::asio::awaitable<backbon::RegisterServiceResp> BackbonClient::RegisterService(ServiceInfo service_info) {
  backbon::RegisterServiceReq request;
  backbon::RegisterServiceResp reply;

  request.set_service(service_info.service);
  request.set_instance_id(service_info.instance_id);
  request.set_address(service_info.address);
  for (const auto &method : service_info.methods) {
    request.add_methods(method);
  }

  grpc::ClientContext context;
  SetDeadline(context, 5);

  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->RegisterService(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });

  if (!status.ok()) {
    spdlog::error("RegisterService failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }

  co_return reply;
}

boost::asio::awaitable<backbon::UnregisterServiceResp> BackbonClient::UnregisterService(
    const std::string &service, const std::string &instance_id) {
  backbon::UnregisterServiceReq request;
  backbon::UnregisterServiceResp reply;
  request.set_service(service);
  request.set_instance_id(instance_id);
  grpc::ClientContext context;
  SetDeadline(context, 5);

  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->UnregisterService(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });

  if (!status.ok()) {
    spdlog::error("UnregisterService failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }

  co_return reply;
}

boost::asio::awaitable<backbon::GetServiceResp> BackbonClient::GetServicesList(const std::string &service_name) {
  backbon::GetServiceReq request;
  backbon::GetServiceResp reply;

  request.set_service(service_name);

  grpc::ClientContext context;
  SetDeadline(context, 3);

  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->GetService(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });

  if (!status.ok()) {
    spdlog::error("GetService failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }

  co_return reply;
}
