#ifndef GROUP_SERVICE_CLIENT_H
#define GROUP_SERVICE_CLIENT_H

#include "group.grpc.pb.h"
#include "group.pb.h"
#include <grpcpp/channel.h>
#include <boost/asio/awaitable.hpp>
#include <memory>
#include <string>
#include <vector>

class GroupClient {
public:
  GroupClient(std::shared_ptr<grpc::Channel> channel);

  boost::asio::awaitable<::group::CreateGroupResp>
  CreateGroup(const ::group::CreateGroupReq &request);

  boost::asio::awaitable<::group::GetGroupInfoResp>
  GetGroupInfo(const ::group::GetGroupInfoReq &request);

  boost::asio::awaitable<::group::GetGroupMembersResp>
  GetGroupMembers(const ::group::GetGroupMembersReq &request);

  boost::asio::awaitable<::group::JoinGroupResp>
  JoinGroup(const ::group::JoinGroupReq &request);

  boost::asio::awaitable<::group::LeaveGroupResp>
  LeaveGroup(const ::group::LeaveGroupReq &request);

  boost::asio::awaitable<::group::InviteMembersResp>
  InviteMembers(const ::group::InviteMembersReq &request);

  boost::asio::awaitable<::group::KickMembersResp>
  KickMembers(const ::group::KickMembersReq &request);

  boost::asio::awaitable<::group::DismissGroupResp>
  DismissGroup(const ::group::DismissGroupReq &request);

  boost::asio::awaitable<::group::UpdateGroupInfoResp>
  UpdateGroupInfo(const ::group::UpdateGroupInfoReq &request);

  boost::asio::awaitable<::group::GetUserGroupsResp>
  GetUserGroups(const ::group::GetUserGroupsReq &request);

  boost::asio::awaitable<::group::GetGroupMemberCountResp>
  GetGroupMemberCount(const ::group::GetGroupMemberCountReq &request);

private:
  std::unique_ptr<group::GroupService::Stub> stub_;
};

#endif
