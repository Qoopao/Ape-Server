#include "services/friend_service/client.h"
#include "util/grpc_async_util.h"
#include "om/otel_trace_propagation.h"
#include <chrono>
#include <stdexcept>

FriendClient::FriendClient(std::shared_ptr<grpc::Channel> channel)
    : m_stub(friendpb::FriendService::NewStub(channel))
{
}

boost::asio::awaitable<friendpb::Reply> FriendClient::Execute(const friendpb::Command& command)
{
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(8));
    ape::otel::InjectTraceContextToGrpcMetadata(context);
    friendpb::Reply reply;
    auto status = co_await ape::grpc_util::GrpcAwait([&](auto&& handler) {
        m_stub->async()->Execute(&context, &command, &reply, std::forward<decltype(handler)>(handler));
    });
    if (!status.ok())
        throw std::runtime_error(status.error_message());
    co_return reply;
}
