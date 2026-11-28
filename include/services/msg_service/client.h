#ifndef MSG_SERVICE_CLIENT_H
#define MSG_SERVICE_CLIENT_H

#include "msg.grpc.pb.h"
#include "msg.pb.h"
#include "sdkws.pb.h"
#include "om/trace_context.h"
#include <boost/asio/awaitable.hpp>
#include <grpcpp/channel.h>
#include <memory>

class MsgClient {
public:
  explicit MsgClient(std::shared_ptr<grpc::Channel> channel)
      : stub_(msg::MessageService::NewStub(channel)) {}

  boost::asio::awaitable<::msg::GetSingleChatHistoryResp> GetSingleChatHistory(
      const ::msg::GetSingleChatHistoryReq& request,
      const ape::otel::TraceContext& trace = {});
  boost::asio::awaitable<::sdkws::SendMessageResp> SendMessages(
      const ::sdkws::SendMessageReq& request,
      const ape::otel::TraceContext& trace = {});

private:
  std::unique_ptr<msg::MessageService::Stub> stub_;
};

#endif
