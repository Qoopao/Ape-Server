#include "services/backbon_service/client.h"
#include "util/grpc_async_util.h"
#include <grpcpp/support/status.h>
#include <spdlog/spdlog.h>

boost::asio::awaitable<backbon::CheckUserOnlineResp> BackbonClient::CheckUserOnline() {
  backbon::CheckUserOnlineReq request;
  backbon::CheckUserOnlineResp reply;
  grpc::ClientContext context;

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
  for (const auto &ipport : service_info.ipports) {
    request.add_ipport(ipport);
  }
  for (const auto &method : service_info.methods) {
    request.add_methods(method);
  }

  grpc::ClientContext context;

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

boost::asio::awaitable<backbon::UnregisterServiceResp> BackbonClient::UnregisterService() {
  backbon::UnregisterServiceReq request;
  backbon::UnregisterServiceResp reply;
  grpc::ClientContext context;

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