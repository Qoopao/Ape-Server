#include "services/group_service/server.h"
#include "util/mysqlhandler.h"
#include "util/redishandler.h"
#include "util/snowflake.h"
#include "util/uuid.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <chrono>

GroupServiceImpl::GroupServiceImpl(const std::string &service_name,
                                   const std::string &listen_address)
    : BaseServiceServer<GroupServiceImpl>(service_name, listen_address) {}

// ── CreateGroup ──

::grpc::ServerUnaryReactor *GroupServiceImpl::CreateGroup(
    ::grpc::CallbackServerContext *context,
    const ::group::CreateGroupReq *request,
    ::group::CreateGroupResp *response) {

  auto reactor = context->DefaultReactor();
  auto groupInfo = request->groupinfo();
  auto initMemberIDs = std::vector<std::string>(
      request->initmemberids().begin(), request->initmemberids().end());

  auto &redis = RedisConnector::instance();
  boost::asio::co_spawn(
      redis.get_executor(),
      [reactor, response, groupInfo = std::move(groupInfo),
       initMemberIDs = std::move(initMemberIDs)]() -> boost::asio::awaitable<void> {
        auto g = groupInfo;

        // 生成 groupID
        if (g.groupid().empty()) {
          g.set_groupid(uuid::newone_str());
        }
        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
        g.set_createtime(now);

        // 确保 owner 在初始成员列表中且 roleLevel=3
        bool ownerInList = false;
        std::vector<sdkws::GroupMemberFullInfo> members;
        for (const auto &uid : initMemberIDs) {
          sdkws::GroupMemberFullInfo m;
          m.set_groupid(g.groupid());
          m.set_userid(uid);
          m.set_jointime(now);
          m.set_joinsource(0);  // 创建时加入
          if (uid == g.owneruserid()) {
            m.set_rolelevel(3);  // 群主
            ownerInList = true;
          } else {
            m.set_rolelevel(1);  // 普通成员
          }
          members.push_back(m);
        }
        if (!ownerInList) {
          sdkws::GroupMemberFullInfo owner;
          owner.set_groupid(g.groupid());
          owner.set_userid(g.owneruserid());
          owner.set_rolelevel(3);
          owner.set_jointime(now);
          owner.set_joinsource(0);
          members.push_back(owner);
        }

        g.set_membercount(static_cast<uint32_t>(members.size()));

        // 写入 groups 表
        bool ok = co_await MySQLHandler::CreateGroup(g);
        if (!ok) {
          response->set_success(false);
          response->set_errmsg("failed to create group in DB");
          reactor->Finish(::grpc::Status::OK);
          co_return;
        }

        // 写入 group_members 表
        ok = co_await MySQLHandler::AddGroupMembers(g.groupid(), members);
        if (!ok) {
          response->set_success(false);
          response->set_errmsg("failed to add group members");
          reactor->Finish(::grpc::Status::OK);
          co_return;
        }

        // 构建 Redis 缓存
        std::vector<std::string> memberIDs;
        for (const auto &m : members) {
          memberIDs.push_back(m.userid());
        }
        co_await RedisHandler::CacheGroupMembers(g.groupid(), memberIDs);

        response->set_success(true);
        response->mutable_groupinfo()->CopyFrom(g);
        reactor->Finish(::grpc::Status::OK);
      },
      boost::asio::detached);

  return reactor;
}

// ── GetGroupInfo ──

::grpc::ServerUnaryReactor *GroupServiceImpl::GetGroupInfo(
    ::grpc::CallbackServerContext *context,
    const ::group::GetGroupInfoReq *request,
    ::group::GetGroupInfoResp *response) {

  auto reactor = context->DefaultReactor();
  std::string groupID = request->groupid();

  auto &redis = RedisConnector::instance();
  boost::asio::co_spawn(
      redis.get_executor(),
      [reactor, response,
       groupID = std::move(groupID)]() -> boost::asio::awaitable<void> {
        auto info = co_await MySQLHandler::GetGroupInfo(groupID);
        if (info.has_value()) {
          response->set_success(true);
          response->mutable_groupinfo()->CopyFrom(*info);
        } else {
          response->set_success(false);
          response->set_errmsg("group not found");
        }
        reactor->Finish(::grpc::Status::OK);
      },
      boost::asio::detached);

  return reactor;
}

// ── GetGroupMembers ──

::grpc::ServerUnaryReactor *GroupServiceImpl::GetGroupMembers(
    ::grpc::CallbackServerContext *context,
    const ::group::GetGroupMembersReq *request,
    ::group::GetGroupMembersResp *response) {

  auto reactor = context->DefaultReactor();
  std::string groupID = request->groupid();
  int offset = request->offset();
  int limit = request->limit();
  if (limit <= 0 || limit > 1000) limit = 500;

  auto &redis = RedisConnector::instance();
  boost::asio::co_spawn(
      redis.get_executor(),
      [reactor, response, groupID = std::move(groupID), offset,
       limit]() -> boost::asio::awaitable<void> {
        auto members =
            co_await MySQLHandler::GetGroupMembers(groupID, offset, limit);
        auto total = co_await MySQLHandler::GetGroupMemberCount(groupID);

        for (const auto &m : members) {
          response->add_members()->CopyFrom(m);
        }
        response->set_total(static_cast<int32_t>(total));
        response->set_success(true);
        reactor->Finish(::grpc::Status::OK);
      },
      boost::asio::detached);

  return reactor;
}

// ── JoinGroup ──

::grpc::ServerUnaryReactor *GroupServiceImpl::JoinGroup(
    ::grpc::CallbackServerContext *context,
    const ::group::JoinGroupReq *request,
    ::group::JoinGroupResp *response) {

  auto reactor = context->DefaultReactor();
  std::string groupID = request->groupid();
  std::string userID = request->userid();

  auto &redis = RedisConnector::instance();
  boost::asio::co_spawn(
      redis.get_executor(),
      [reactor, response, groupID = std::move(groupID),
       userID = std::move(userID),
       joinSource = request->joinsource()]() -> boost::asio::awaitable<void> {
        // 检查群是否存在
        auto info = co_await MySQLHandler::GetGroupInfo(groupID);
        if (!info.has_value() || info->status() != 0) {
          response->set_success(false);
          response->set_errmsg("group not found or dismissed");
          reactor->Finish(::grpc::Status::OK);
          co_return;
        }

        // 检查是否已在群内
        bool alreadyIn =
            co_await MySQLHandler::IsUserInGroup(groupID, userID);
        if (alreadyIn) {
          response->set_success(false);
          response->set_errmsg("user already in group");
          reactor->Finish(::grpc::Status::OK);
          co_return;
        }

        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();

        sdkws::GroupMemberFullInfo member;
        member.set_groupid(groupID);
        member.set_userid(userID);
        member.set_rolelevel(1);
        member.set_jointime(now);
        member.set_joinsource(joinSource);

        std::vector<sdkws::GroupMemberFullInfo> members{member};
        bool ok = co_await MySQLHandler::AddGroupMembers(groupID, members);
        if (!ok) {
          response->set_success(false);
          response->set_errmsg("failed to add member");
          reactor->Finish(::grpc::Status::OK);
          co_return;
        }

        co_await MySQLHandler::UpdateGroupMemberCount(groupID, 1);
        co_await RedisHandler::InvalidateGroupCache(groupID);
        response->set_success(true);
        reactor->Finish(::grpc::Status::OK);
      },
      boost::asio::detached);

  return reactor;
}

// ── LeaveGroup ──

::grpc::ServerUnaryReactor *GroupServiceImpl::LeaveGroup(
    ::grpc::CallbackServerContext *context,
    const ::group::LeaveGroupReq *request,
    ::group::LeaveGroupResp *response) {

  auto reactor = context->DefaultReactor();
  std::string groupID = request->groupid();
  std::string userID = request->userid();

  auto &redis = RedisConnector::instance();
  boost::asio::co_spawn(
      redis.get_executor(),
      [reactor, response, groupID = std::move(groupID),
       userID = std::move(userID)]() -> boost::asio::awaitable<void> {
        auto info = co_await MySQLHandler::GetGroupInfo(groupID);
        if (!info.has_value()) {
          response->set_success(false);
          response->set_errmsg("group not found");
          reactor->Finish(::grpc::Status::OK);
          co_return;
        }

        bool ok = co_await MySQLHandler::RemoveGroupMembers(
            groupID, std::vector<std::string>{userID});
        if (!ok) {
          response->set_success(false);
          response->set_errmsg("failed to remove member");
          reactor->Finish(::grpc::Status::OK);
          co_return;
        }

        co_await MySQLHandler::UpdateGroupMemberCount(groupID, -1);
        co_await RedisHandler::InvalidateGroupCache(groupID);
        response->set_success(true);
        reactor->Finish(::grpc::Status::OK);
      },
      boost::asio::detached);

  return reactor;
}

// ── InviteMembers ──

::grpc::ServerUnaryReactor *GroupServiceImpl::InviteMembers(
    ::grpc::CallbackServerContext *context,
    const ::group::InviteMembersReq *request,
    ::group::InviteMembersResp *response) {

  auto reactor = context->DefaultReactor();
  std::string groupID = request->groupid();
  auto userIDs = std::vector<std::string>(request->userids().begin(),
                                           request->userids().end());
  std::string inviterUserID = request->inviteruserid();

  auto &redis = RedisConnector::instance();
  boost::asio::co_spawn(
      redis.get_executor(),
      [reactor, response, groupID = std::move(groupID),
       userIDs = std::move(userIDs),
       inviterUserID = std::move(inviterUserID)]() -> boost::asio::awaitable<void> {
        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();

        int added = 0;
        std::vector<std::string> failedIDs;
        for (const auto &uid : userIDs) {
          bool inGroup =
              co_await MySQLHandler::IsUserInGroup(groupID, uid);
          if (inGroup) {
            failedIDs.push_back(uid);
            continue;
          }

          sdkws::GroupMemberFullInfo m;
          m.set_groupid(groupID);
          m.set_userid(uid);
          m.set_rolelevel(1);
          m.set_jointime(now);
          m.set_joinsource(0);  // 邀请
          m.set_inviteruserid(inviterUserID);

          std::vector<sdkws::GroupMemberFullInfo> members{m};
          bool ok = co_await MySQLHandler::AddGroupMembers(groupID, members);
          if (ok) {
            added++;
          } else {
            failedIDs.push_back(uid);
          }
        }

        if (added > 0) {
          co_await MySQLHandler::UpdateGroupMemberCount(groupID, added);
          co_await RedisHandler::InvalidateGroupCache(groupID);
        }

        response->set_success(failedIDs.empty());
        for (const auto &fid : failedIDs) {
          response->add_faileduserids(fid);
        }
        if (!failedIDs.empty()) {
          response->set_errmsg("some users failed to be added");
        }
        reactor->Finish(::grpc::Status::OK);
      },
      boost::asio::detached);

  return reactor;
}

// ── KickMembers ──

::grpc::ServerUnaryReactor *GroupServiceImpl::KickMembers(
    ::grpc::CallbackServerContext *context,
    const ::group::KickMembersReq *request,
    ::group::KickMembersResp *response) {

  auto reactor = context->DefaultReactor();
  std::string groupID = request->groupid();
  auto userIDs = std::vector<std::string>(request->userids().begin(),
                                           request->userids().end());

  auto &redis = RedisConnector::instance();
  boost::asio::co_spawn(
      redis.get_executor(),
      [reactor, response, groupID = std::move(groupID),
       userIDs = std::move(userIDs)]() -> boost::asio::awaitable<void> {
        bool ok = co_await MySQLHandler::RemoveGroupMembers(groupID, userIDs);
        if (ok) {
          co_await MySQLHandler::UpdateGroupMemberCount(
              groupID, -static_cast<int32_t>(userIDs.size()));
          co_await RedisHandler::InvalidateGroupCache(groupID);
        }

        response->set_success(ok);
        if (!ok) {
          response->set_errmsg("failed to kick members");
        }
        reactor->Finish(::grpc::Status::OK);
      },
      boost::asio::detached);

  return reactor;
}

// ── DismissGroup ──

::grpc::ServerUnaryReactor *GroupServiceImpl::DismissGroup(
    ::grpc::CallbackServerContext *context,
    const ::group::DismissGroupReq *request,
    ::group::DismissGroupResp *response) {

  auto reactor = context->DefaultReactor();
  std::string groupID = request->groupid();

  auto &redis = RedisConnector::instance();
  boost::asio::co_spawn(
      redis.get_executor(),
      [reactor, response,
       groupID = std::move(groupID)]() -> boost::asio::awaitable<void> {
        bool ok = co_await MySQLHandler::DismissGroup(groupID);
        if (ok) {
          co_await RedisHandler::InvalidateGroupCache(groupID);
        }

        response->set_success(ok);
        if (!ok) {
          response->set_errmsg("failed to dismiss group");
        }
        reactor->Finish(::grpc::Status::OK);
      },
      boost::asio::detached);

  return reactor;
}

// ── UpdateGroupInfo ──

::grpc::ServerUnaryReactor *GroupServiceImpl::UpdateGroupInfo(
    ::grpc::CallbackServerContext *context,
    const ::group::UpdateGroupInfoReq *request,
    ::group::UpdateGroupInfoResp *response) {

  auto reactor = context->DefaultReactor();
  std::string groupID = request->groupid();
  auto infoForSet = request->groupinfo();

  auto &redis = RedisConnector::instance();
  boost::asio::co_spawn(
      redis.get_executor(),
      [reactor, response, groupID = std::move(groupID),
       infoForSet = std::move(infoForSet)]() -> boost::asio::awaitable<void> {
        bool ok =
            co_await MySQLHandler::UpdateGroupInfo(groupID, infoForSet);
        response->set_success(ok);
        if (!ok) {
          response->set_errmsg("failed to update group info");
        }
        reactor->Finish(::grpc::Status::OK);
      },
      boost::asio::detached);

  return reactor;
}

// ── GetUserGroups ──

::grpc::ServerUnaryReactor *GroupServiceImpl::GetUserGroups(
    ::grpc::CallbackServerContext *context,
    const ::group::GetUserGroupsReq *request,
    ::group::GetUserGroupsResp *response) {

  auto reactor = context->DefaultReactor();
  std::string userID = request->userid();

  auto &redis = RedisConnector::instance();
  boost::asio::co_spawn(
      redis.get_executor(),
      [reactor, response,
       userID = std::move(userID)]() -> boost::asio::awaitable<void> {
        auto groupIDs = co_await MySQLHandler::GetUserGroupIDs(userID);
        for (const auto &gid : groupIDs) {
          auto info = co_await MySQLHandler::GetGroupInfo(gid);
          if (info.has_value()) {
            response->add_groups()->CopyFrom(*info);
          }
        }
        response->set_success(true);
        reactor->Finish(::grpc::Status::OK);
      },
      boost::asio::detached);

  return reactor;
}

// ── GetGroupMemberCount ──

::grpc::ServerUnaryReactor *GroupServiceImpl::GetGroupMemberCount(
    ::grpc::CallbackServerContext *context,
    const ::group::GetGroupMemberCountReq *request,
    ::group::GetGroupMemberCountResp *response) {

  auto reactor = context->DefaultReactor();
  std::string groupID = request->groupid();

  auto &redis = RedisConnector::instance();
  boost::asio::co_spawn(
      redis.get_executor(),
      [reactor, response,
       groupID = std::move(groupID)]() -> boost::asio::awaitable<void> {
        // 优先 Redis 缓存
        auto count = co_await RedisHandler::GetGroupMemberCountFromCache(groupID);
        if (count == 0) {
          count = co_await MySQLHandler::GetGroupMemberCount(groupID);
        }
        response->set_success(true);
        response->set_count(count);
        reactor->Finish(::grpc::Status::OK);
      },
      boost::asio::detached);

  return reactor;
}
