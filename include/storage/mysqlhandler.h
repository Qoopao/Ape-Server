#ifndef MYSQLHANDLER_H
#define MYSQLHANDLER_H

#include "user/userinfo.h"

#include <boost/asio/awaitable.hpp>
#include <optional>
#include <string>
#include <vector>

// Forward declarations for protobuf types
namespace sdkws {
class GroupInfo;
class GroupInfoForSet;
class GroupMemberFullInfo;
} // namespace sdkws

// ============================================================================
// MySQLHandler — MySQL 异步操作封装
//
// 所有方法均为协程方法（boost::asio::awaitable<T>），
// 内部通过 co_await MySQLConnector::async_execute() 异步执行 SQL。
// ============================================================================

class MySQLHandler
{
public:
    // ── 用户相关 ──
    static boost::asio::awaitable<std::optional<userInfo>>
        insert_user(const userInfo &user);

    static boost::asio::awaitable<std::optional<userInfo>>
        find_user_by_username(const std::string &username);

    static boost::asio::awaitable<std::optional<userInfo>>
        find_user_by_id(const std::string &user_id);

    static boost::asio::awaitable<bool>
        update_last_login(const std::string &user_id);

    static boost::asio::awaitable<bool>
        username_exists(const std::string &username);

    static boost::asio::awaitable<std::optional<userInfo>>
        get_user_online_info(const std::string &user_id);

    // ── 群组 CRUD ──
    static boost::asio::awaitable<bool>
        CreateGroup(const sdkws::GroupInfo &group);

    static boost::asio::awaitable<std::optional<sdkws::GroupInfo>>
        GetGroupInfo(const std::string &group_id);

    static boost::asio::awaitable<bool>
        UpdateGroupInfo(const std::string &group_id,
                        const sdkws::GroupInfoForSet &update);

    static boost::asio::awaitable<bool>
        DismissGroup(const std::string &group_id);

    static boost::asio::awaitable<bool>
        AddGroupMembers(const std::string &group_id,
                        const std::vector<sdkws::GroupMemberFullInfo> &members);

    static boost::asio::awaitable<bool>
        RemoveGroupMembers(const std::string &group_id,
                           const std::vector<std::string> &user_ids);

    static boost::asio::awaitable<bool>
        UpdateGroupMemberCount(const std::string &group_id, int32_t delta);

    static boost::asio::awaitable<std::vector<sdkws::GroupMemberFullInfo>>
        GetGroupMembers(const std::string &group_id, int offset = 0,
                        int limit = 500);

    static boost::asio::awaitable<int64_t>
        GetGroupMemberCount(const std::string &group_id);

    static boost::asio::awaitable<std::vector<std::string>>
        GetUserGroupIDs(const std::string &user_id);

    static boost::asio::awaitable<bool>
        IsUserInGroup(const std::string &group_id, const std::string &user_id);
};

#endif