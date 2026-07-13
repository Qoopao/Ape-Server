#ifndef GROUP_SERVICE_SERVER_H
#define GROUP_SERVICE_SERVER_H

#include "group.grpc.pb.h"
#include "group.pb.h"
#include "services/base_service.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/status.h>
#include <spdlog/spdlog.h>
#include <string>

class GroupServiceImpl final : public group::GroupService::CallbackService,
                               public BaseServiceServer<GroupServiceImpl> {
public:
  GroupServiceImpl(const std::string &service_name,
                   const std::string &listen_address);
  ~GroupServiceImpl() override = default;

  ::grpc::ServerUnaryReactor *
  CreateGroup(::grpc::CallbackServerContext *context,
              const ::group::CreateGroupReq *request,
              ::group::CreateGroupResp *response) override;

  ::grpc::ServerUnaryReactor *
  GetGroupInfo(::grpc::CallbackServerContext *context,
               const ::group::GetGroupInfoReq *request,
               ::group::GetGroupInfoResp *response) override;

  ::grpc::ServerUnaryReactor *
  GetGroupMembers(::grpc::CallbackServerContext *context,
                  const ::group::GetGroupMembersReq *request,
                  ::group::GetGroupMembersResp *response) override;

  ::grpc::ServerUnaryReactor *
  JoinGroup(::grpc::CallbackServerContext *context,
            const ::group::JoinGroupReq *request,
            ::group::JoinGroupResp *response) override;

  ::grpc::ServerUnaryReactor *
  LeaveGroup(::grpc::CallbackServerContext *context,
             const ::group::LeaveGroupReq *request,
             ::group::LeaveGroupResp *response) override;

  ::grpc::ServerUnaryReactor *
  InviteMembers(::grpc::CallbackServerContext *context,
                const ::group::InviteMembersReq *request,
                ::group::InviteMembersResp *response) override;

  ::grpc::ServerUnaryReactor *
  KickMembers(::grpc::CallbackServerContext *context,
              const ::group::KickMembersReq *request,
              ::group::KickMembersResp *response) override;

  ::grpc::ServerUnaryReactor *
  DismissGroup(::grpc::CallbackServerContext *context,
               const ::group::DismissGroupReq *request,
               ::group::DismissGroupResp *response) override;

  ::grpc::ServerUnaryReactor *
  UpdateGroupInfo(::grpc::CallbackServerContext *context,
                  const ::group::UpdateGroupInfoReq *request,
                  ::group::UpdateGroupInfoResp *response) override;

  ::grpc::ServerUnaryReactor *
  GetUserGroups(::grpc::CallbackServerContext *context,
                const ::group::GetUserGroupsReq *request,
                ::group::GetUserGroupsResp *response) override;

  ::grpc::ServerUnaryReactor *
  GetGroupMemberCount(::grpc::CallbackServerContext *context,
                      const ::group::GetGroupMemberCountReq *request,
                      ::group::GetGroupMemberCountResp *response) override;
};

#endif
