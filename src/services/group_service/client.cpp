#include "services/group_service/client.h"
#include "util/grpc_async_util.h"
#include "om/otel_trace_propagation.h"
#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>
#include <spdlog/spdlog.h>

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
    if(!status.ok()) { reply.Clear(); reply.set_errorcode("DB_ERROR"); reply.set_errormessage("Group service unavailable"); spdlog::error("GroupClient.Execute: {}",status.error_message()); }
    co_return reply;
}
