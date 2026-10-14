#include "services/push_service/client.h"
#include "util/grpc_async_util.h"
#include <grpcpp/support/status.h>
#include <spdlog/spdlog.h>
#include <chrono>
#include <stdexcept>

boost::asio::awaitable<::push::PushMsgResp> PushClient::PushMsg(const ::push::PushMsgReq& request) {
  ::push::PushMsgResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->PushMsg(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("PushMsg failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::push::DelUserPushTokenResp> PushClient::DelUserPushToken(const ::push::DelUserPushTokenReq& request) {
  ::push::DelUserPushTokenResp reply;
  grpc::ClientContext context;
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->DelUserPushToken(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("DelUserPushToken failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}

boost::asio::awaitable<::push::AckMsgResp> PushClient::AckMsg(const ::push::AckMsgReq& request) {
  ::push::AckMsgResp reply;
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(8));
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->AckMsg(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("AckMsg failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
    throw std::runtime_error(status.error_message());
  }
  co_return reply;
}
