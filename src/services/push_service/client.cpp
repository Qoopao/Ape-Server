#include "services/push_service/client.h"
#include "om/trace_context.h"
#include "util/grpc_async_util.h"
#include <chrono>
#include <spdlog/spdlog.h>
#include <stdexcept>

boost::asio::awaitable<::push::AckConversationsResp>
PushClient::AckConversations(
    const ::push::AckConversationsReq& request,
    const ape::otel::TraceContext& trace) {
  ::push::AckConversationsResp reply;
  grpc::ClientContext context;
  ape::otel::InjectTraceContext(
      context, trace.valid() ? trace : ape::otel::CurrentTraceContext());
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(8));
  auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
    stub_->async()->AckConversations(&context, &request, &reply,
        std::forward<decltype(handler)>(handler));
  });
  if (!status.ok()) {
    spdlog::error("AckConversations failed: {} {}",
                  static_cast<int>(status.error_code()),
                  status.error_message());
    throw std::runtime_error(status.error_message());
  }
  co_return reply;
}
