#ifndef PUSH_SERVICE_CLIENT_H
#define PUSH_SERVICE_CLIENT_H

#include "push.grpc.pb.h"
#include "push.pb.h"
#include "om/trace_context.h"
#include <grpcpp/channel.h>
#include <boost/asio/awaitable.hpp>
#include <memory>

class PushClient {
public:
  explicit PushClient(std::shared_ptr<grpc::Channel> channel)
      : stub_(push::PushService::NewStub(channel)) {}

  // 网关 ACK 汇总入口：type=106 的会话水位。
  boost::asio::awaitable<::push::AckConversationsResp> AckConversations(
      const ::push::AckConversationsReq& request,
      const ape::otel::TraceContext& trace = {});

private:
  std::unique_ptr<push::PushService::Stub> stub_;
};

#endif
