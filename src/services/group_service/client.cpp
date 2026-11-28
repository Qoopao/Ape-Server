#include "services/group_service/client.h"
#include "util/grpc_async_util.h"
#include "om/otel_trace_propagation.h"
#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>
#include <spdlog/spdlog.h>

#include <set>
#include <string>

namespace {

const std::set<std::string> &GroupErrorCodes() {
  static const std::set<std::string> codes = {
      "INVALID_PARAM", "INVALID_GROUP", "INVALID_REQUEST_ID", "INVALID_NAME",
      "INVALID_ACCOUNT", "GROUP_NOT_FOUND", "GROUP_DISMISSED", "NOT_MEMBER",
      "OWNER_REQUIRED", "OWNER_CANNOT_LEAVE", "ALREADY_JOINED",
      "APPLICATION_NOT_FOUND", "APPLICATION_HANDLED", "MEMBERSHIP_CHANGED",
      "NAME_UNCHANGED", "EMPTY_MESSAGE", "MESSAGE_TOO_LARGE",
      "IDEMPOTENCY_CONFLICT", "RATE_LIMITED", "GROUP_FULL",
      "UNSUPPORTED_OPERATION", "DB_ERROR", "CACHE_ERROR", "SERVICE_UNAVAILABLE"};
  return codes;
}

// GroupService 把业务错误码编码为 "CODE" 或 "CODE|说明"。
std::string DecodeGroupError(const std::string &status) {
  const auto separator = status.find('|');
  const auto code = status.substr(0, separator);
  if (GroupErrorCodes().contains(code))
    return code;
  return "SERVICE_UNAVAILABLE";
}

std::string DecodeGroupErrorMessage(const std::string &status) {
  const auto separator = status.find('|');
  if (separator == std::string::npos)
    return status;
  return status.substr(separator + 1);
}

} // namespace

GroupClient::GroupClient(std::shared_ptr<grpc::Channel> channel)
    : stub_(groupchat::GroupService::NewStub(channel)) {}

boost::asio::awaitable<groupchat::Reply> GroupClient::Execute(const groupchat::Command& request) {
    grpc::ClientContext context;
    ape::otel::InjectTraceContextToGrpcMetadata(context);
    context.set_deadline(std::chrono::system_clock::now()+std::chrono::seconds(10));
    groupchat::Reply reply;
    auto status=co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
        stub_->async()->Execute(&context,&request,&reply,std::forward<decltype(handler)>(handler));
    });
    if(!status.ok()) { reply.Clear(); reply.set_errorcode("DB_ERROR"); reply.set_errormessage("Group service unavailable"); spdlog::error("GroupClient.Execute: code={} message={}",static_cast<int>(status.error_code()),status.error_message()); }
    co_return reply;
}

boost::asio::awaitable<groupchat::SendGroupMessageResp> GroupClient::SendGroupMessages(
    const groupchat::SendGroupMessageReq& request,
    const ape::otel::TraceContext& trace) {
    grpc::ClientContext context;
    ape::otel::InjectTraceContext(
        context, trace.valid() ? trace : ape::otel::CurrentTraceContext());
    context.set_deadline(std::chrono::system_clock::now()+std::chrono::seconds(3));
    groupchat::SendGroupMessageResp reply;
    auto status=co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
        stub_->async()->SendGroupMessages(&context,&request,&reply,std::forward<decltype(handler)>(handler));
    });
    if(!status.ok()) {
        reply.Clear();
        reply.set_errorcode("SERVICE_UNAVAILABLE");
        reply.set_errormsg("群服务暂时不可用，请稍后重试");
        spdlog::error("GroupClient.SendGroupMessages: code={} message={}",
            static_cast<int>(status.error_code()),status.error_message());
    }
    co_return reply;
}

boost::asio::awaitable<GroupHistoryResult> GroupClient::GetGroupChatHistory(
    const groupchat::GetGroupChatHistoryReq& request,
    const ape::otel::TraceContext& trace) {
    grpc::ClientContext context;
    ape::otel::InjectTraceContext(
        context, trace.valid() ? trace : ape::otel::CurrentTraceContext());
    context.set_deadline(std::chrono::system_clock::now()+std::chrono::seconds(5));
    GroupHistoryResult result;
    auto status=co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
        stub_->async()->GetGroupChatHistory(
            &context,&request,&result.history,std::forward<decltype(handler)>(handler));
    });
    if(!status.ok()) {
        result.history.Clear();
        result.errorCode = DecodeGroupError(status.error_message());
        result.errorMessage = DecodeGroupErrorMessage(status.error_message());
        spdlog::error("GroupClient.GetGroupChatHistory: code={} message={}",
            static_cast<int>(status.error_code()),status.error_message());
        co_return result;
    }
    result.errorCode = "0";
    co_return result;
}
