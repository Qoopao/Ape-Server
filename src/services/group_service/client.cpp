#include "services/group_service/client.h"
#include "util/grpc_async_util.h"
#include "om/otel_trace_propagation.h"
#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>
#include <spdlog/spdlog.h>

GroupClient::GroupClient(std::shared_ptr<grpc::Channel> channel)
    : stub_(group::GroupService::NewStub(channel)) {}

#define GROUP_CLIENT_METHOD(Method, Req, Resp)                                 \
  boost::asio::awaitable<::group::Resp> GroupClient::Method(                   \
      const ::group::Req &request) {                                           \
    grpc::ClientContext ctx;                                                   \
    ape::otel::InjectTraceContextToGrpcMetadata(ctx);                          \
    ctx.set_deadline(std::chrono::system_clock::now() +                        \
                     std::chrono::seconds(5));                                 \
    ::group::Resp resp;                                                        \
    auto status = co_await ape::grpc_util::GrpcAwait([&](auto &&handler) {    \
      stub_->async()->Method(&ctx, &request, &resp,                            \
                             std::forward<decltype(handler)>(handler));        \
    });                                                                        \
    if (!status.ok()) {                                                        \
      spdlog::error("GroupClient::" #Method ": gRPC error={}",                 \
                    status.error_message());                                   \
    }                                                                          \
    co_return resp;                                                            \
  }

GROUP_CLIENT_METHOD(CreateGroup, CreateGroupReq, CreateGroupResp)
GROUP_CLIENT_METHOD(GetGroupInfo, GetGroupInfoReq, GetGroupInfoResp)
GROUP_CLIENT_METHOD(GetGroupMembers, GetGroupMembersReq, GetGroupMembersResp)
GROUP_CLIENT_METHOD(JoinGroup, JoinGroupReq, JoinGroupResp)
GROUP_CLIENT_METHOD(LeaveGroup, LeaveGroupReq, LeaveGroupResp)
GROUP_CLIENT_METHOD(InviteMembers, InviteMembersReq, InviteMembersResp)
GROUP_CLIENT_METHOD(KickMembers, KickMembersReq, KickMembersResp)
GROUP_CLIENT_METHOD(DismissGroup, DismissGroupReq, DismissGroupResp)
GROUP_CLIENT_METHOD(UpdateGroupInfo, UpdateGroupInfoReq, UpdateGroupInfoResp)
GROUP_CLIENT_METHOD(GetUserGroups, GetUserGroupsReq, GetUserGroupsResp)
GROUP_CLIENT_METHOD(GetGroupMemberCount, GetGroupMemberCountReq,
                     GetGroupMemberCountResp)

#undef GROUP_CLIENT_METHOD
