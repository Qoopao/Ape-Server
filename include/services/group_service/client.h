#ifndef GROUP_SERVICE_CLIENT_H
#define GROUP_SERVICE_CLIENT_H

#include "groupchat.grpc.pb.h"
#include "groupchat.pb.h"
#include "om/trace_context.h"
#include <grpcpp/channel.h>
#include <boost/asio/awaitable.hpp>
#include <memory>
#include <string>

// 245 响应没有 errorCode 字段：业务错误码由 GroupService 放在 gRPC status message
// （"CODE|说明"）里，客户端还原成外层 SdkWSResp.errorCode。
struct GroupHistoryResult {
  std::string errorCode;
  std::string errorMessage;
  groupchat::GetGroupChatHistoryResp history;
};

class GroupClient {
public:
  GroupClient(std::shared_ptr<grpc::Channel> channel);
  boost::asio::awaitable<groupchat::Reply> Execute(const groupchat::Command& request);
  boost::asio::awaitable<groupchat::SendGroupMessageResp> SendGroupMessages(
      const groupchat::SendGroupMessageReq& request,
      const ape::otel::TraceContext& trace = {});
  boost::asio::awaitable<GroupHistoryResult> GetGroupChatHistory(
      const groupchat::GetGroupChatHistoryReq& request,
      const ape::otel::TraceContext& trace = {});

private:
  std::unique_ptr<groupchat::GroupService::Stub> stub_;
};

#endif
