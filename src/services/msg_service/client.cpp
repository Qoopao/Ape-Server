#include "services/msg_service/client.h"
#include "util/grpc_async_util.h"
#include "om/otel_trace_propagation.h"
#include <chrono>
#include <spdlog/spdlog.h>
#include <stdexcept>

boost::asio::awaitable<::msg::GetSingleChatHistoryResp>
MsgClient::GetSingleChatHistory(
    const ::msg::GetSingleChatHistoryReq& request,
    const ape::otel::TraceContext& trace) {
  ::msg::GetSingleChatHistoryResp reply;
  grpc::ClientContext context;
  ape::otel::InjectTraceContext(
      context, trace.valid() ? trace : ape::otel::CurrentTraceContext());
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(10));
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->GetSingleChatHistory(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok())
    throw std::runtime_error(status.error_message());
  co_return reply;
}

boost::asio::awaitable<::sdkws::SendMessageResp> MsgClient::SendMessages(
    const ::sdkws::SendMessageReq& request,
    const ape::otel::TraceContext& trace) {
  ::sdkws::SendMessageResp reply;
  grpc::ClientContext context;
  ape::otel::InjectTraceContext(
      context, trace.valid() ? trace : ape::otel::CurrentTraceContext());
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->SendMessages(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("SendMessages failed: {} {}", static_cast<int>(status.error_code()),
                  status.error_message());
  }
  co_return reply;
}
