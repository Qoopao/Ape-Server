#include "storage/mysqlhandler.h"
#include "storage/mysqlconnector.h"
#include "sdkws.pb.h"

#include <boost/asio/use_awaitable.hpp>
#include <boost/mysql/error_with_diagnostics.hpp>
#include <boost/mysql/results.hpp>
#include <boost/mysql/with_params.hpp>
#include <iomanip>
#include <sstream>
#include <spdlog/spdlog.h>

// ═══════════════════════════════════════════════════════════════════════════
// 辅助：将 results.rows() 的第一行解析为 userInfo
// 字段顺序：user_id, username, nickname, password_hash, password_salt
// ═══════════════════════════════════════════════════════════════════════════
namespace {

userInfo parse_user_from_row(const boost::mysql::rows_view &rows)
{
    auto row = rows.at(0);
    userInfo user;
    user.user_id = row.at(0).as_string();
    user.username = row.at(1).as_string();
    user.nickname = row.at(2).as_string();
    user.password_hash = row.at(3).as_string();
    user.password_salt = row.at(4).as_string();
    // last_login_at 在 SELECT 查询中位于第 8 列 (0-based index 7)
    // 类型是 TIMESTAMP，Boost.MySQL 返回 datetime 类型，不是 string，
    // 需要先用 as_datetime() 再手动格式化为字符串
    if (row.size() > 7 && !row.at(7).is_null())
    {
        auto dt = row.at(7).as_datetime();
        std::ostringstream oss;
        oss << static_cast<int>(dt.year()) << '-'
            << std::setw(2) << std::setfill('0') << static_cast<int>(dt.month()) << '-'
            << std::setw(2) << std::setfill('0') << static_cast<int>(dt.day()) << ' '
            << std::setw(2) << std::setfill('0') << static_cast<int>(dt.hour()) << ':'
            << std::setw(2) << std::setfill('0') << static_cast<int>(dt.minute()) << ':'
            << std::setw(2) << std::setfill('0') << static_cast<int>(dt.second());
        user.last_login_at = oss.str();
    }
    return user;
}

} // namespace

// ── 插入用户 ──

boost::asio::awaitable<std::optional<userInfo>>
MySQLHandler::insert_user(const userInfo &user)
{
    try
    {
        auto stmt = boost::mysql::with_params(
            "INSERT INTO users (user_id, username, nickname, password_hash, password_salt) "
            "VALUES ({}, {}, {}, {}, {})",
            user.user_id,
            user.username,
            user.nickname,
            user.password_hash,
            user.password_salt);

        co_await MySQLConnector::instance().async_execute(stmt);

        spdlog::info("Inserted user: {} ({})", user.username, user.user_id);
        co_return user;
    }
    catch (const std::exception &e)
    {
        spdlog::error("Insert user failed: {}", e.what());
        co_return std::nullopt;
    }
}

// ── 通过用户名查找用户 ──

boost::asio::awaitable<std::optional<userInfo>>
MySQLHandler::find_user_by_username(const std::string &username)
{
    try
    {
        auto stmt = boost::mysql::with_params(
            "SELECT user_id, username, nickname, password_hash, password_salt, "
            "created_at, updated_at, last_login_at FROM users WHERE username = {}",
            username);

        boost::mysql::results result = co_await MySQLConnector::instance().async_execute(stmt);
        auto rows = result.rows();
        if (rows.empty())
        {
            spdlog::warn("User '{}' not found", username);
            co_return std::nullopt;
        }

        auto user = parse_user_from_row(rows);
        co_return user;
    }
    catch (const std::exception &e)
    {
        spdlog::error("Find user by username failed: {}", e.what());
        co_return std::nullopt;
    }
}

// ── 通过 user_id 查找用户 ──

boost::asio::awaitable<std::optional<userInfo>>
MySQLHandler::find_user_by_id(const std::string &user_id)
{
    try
    {
        auto stmt = boost::mysql::with_params(
            "SELECT user_id, username, nickname, password_hash, password_salt, "
            "created_at, updated_at, last_login_at FROM users WHERE user_id = {}",
            user_id);

        boost::mysql::results result = co_await MySQLConnector::instance().async_execute(stmt);
        auto rows = result.rows();
        if (rows.empty())
        {
            spdlog::warn("User '{}' not found", user_id);
            co_return std::nullopt;
        }

        auto user = parse_user_from_row(rows);
        co_return user;
    }
    catch (const std::exception &e)
    {
        spdlog::error("Find user by id failed: {}", e.what());
        co_return std::nullopt;
    }
}

// ── 更新最后登录时间 ──

boost::asio::awaitable<bool>
MySQLHandler::update_last_login(const std::string &user_id)
{
    try
    {
        auto stmt = boost::mysql::with_params(
            "UPDATE users SET last_login_at = NOW() WHERE user_id = {}", user_id);

        co_await MySQLConnector::instance().async_execute(stmt);
        co_return true;
    }
    catch (const std::exception &e)
    {
        spdlog::error("Update last_login failed: {}", e.what());
        co_return false;
    }
}

// ── 检查用户名是否已存在 ──

boost::asio::awaitable<bool>
MySQLHandler::username_exists(const std::string &username)
{
    try
    {
        auto stmt = boost::mysql::with_params(
            "SELECT COUNT(*) as cnt FROM users WHERE username = {}", username);

        boost::mysql::results result = co_await MySQLConnector::instance().async_execute(stmt);
        auto rows = result.rows();
        if (!rows.empty())
        {
            int64_t count = rows.at(0).at(0).as_int64();
            co_return count > 0;
        }
        co_return false;
    }
    catch (const std::exception &e)
    {
        spdlog::error("Check username exists failed: {}", e.what());
        co_return false;
    }
}

// ── 获取用户在线信息 ──

boost::asio::awaitable<std::optional<userInfo>>
MySQLHandler::get_user_online_info(const std::string &user_id)
{
    try
    {
        auto stmt = boost::mysql::with_params(
            "SELECT user_id, username, nickname, password_hash, password_salt, "
            "created_at, updated_at, last_login_at FROM users WHERE user_id = {}",
            user_id);

        boost::mysql::results result = co_await MySQLConnector::instance().async_execute(stmt);
        auto rows = result.rows();
        if (rows.empty())
        {
            spdlog::warn("User '{}' not found (get_user_online_info)", user_id);
            co_return std::nullopt;
        }

        auto user = parse_user_from_row(rows);
        co_return user;
    }
    catch (const std::exception &e)
    {
        spdlog::error("Get user online info failed: {}", e.what());
        co_return std::nullopt;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 群组 CRUD
// ═══════════════════════════════════════════════════════════════════════════

namespace {

sdkws::GroupInfo parse_group_from_row(const boost::mysql::rows_view &rows)
{
    auto row = rows.at(0);
    sdkws::GroupInfo g;
    g.set_groupid(std::string(row.at(0).as_string()));
    g.set_groupname(std::string(row.at(1).as_string()));
    if (!row.at(2).is_null()) g.set_notification(std::string(row.at(2).as_string()));
    if (!row.at(3).is_null()) g.set_introduction(std::string(row.at(3).as_string()));
    if (!row.at(4).is_null()) g.set_faceurl(std::string(row.at(4).as_string()));
    g.set_owneruserid(std::string(row.at(5).as_string()));
    g.set_creatoruserid(std::string(row.at(6).as_string()));
    g.set_createtime(row.at(7).as_int64());
    g.set_membercount(static_cast<uint32_t>(row.at(8).as_int64()));
    g.set_status(row.at(9).as_int64());
    g.set_grouptype(row.at(10).as_int64());
    g.set_needverification(row.at(11).as_int64());
    g.set_lookmemberinfo(row.at(12).as_int64());
    g.set_applymemberfriend(row.at(13).as_int64());
    if (!row.at(14).is_null()) g.set_notificationupdatetime(row.at(14).as_int64());
    if (!row.at(15).is_null()) g.set_notificationuserid(std::string(row.at(15).as_string()));
    if (!row.at(16).is_null()) g.set_ex(std::string(row.at(16).as_string()));
    return g;
}

sdkws::GroupMemberFullInfo parse_member_from_row(const boost::mysql::rows_view &rows, size_t idx)
{
    auto row = rows.at(idx);
    sdkws::GroupMemberFullInfo m;
    m.set_groupid(std::string(row.at(0).as_string()));
    m.set_userid(std::string(row.at(1).as_string()));
    m.set_rolelevel(row.at(2).as_int64());
    m.set_jointime(row.at(3).as_int64());
    if (!row.at(4).is_null()) m.set_nickname(std::string(row.at(4).as_string()));
    if (!row.at(5).is_null()) m.set_faceurl(std::string(row.at(5).as_string()));
    m.set_joinsource(row.at(6).as_int64());
    if (!row.at(7).is_null()) m.set_operatoruserid(std::string(row.at(7).as_string()));
    if (!row.at(8).is_null()) m.set_inviteruserid(std::string(row.at(8).as_string()));
    if (!row.at(9).is_null()) m.set_muteendtime(row.at(9).as_int64());
    if (!row.at(10).is_null()) m.set_ex(std::string(row.at(10).as_string()));
    return m;
}

} // namespace

// ── 创建群组 ──

boost::asio::awaitable<bool>
MySQLHandler::CreateGroup(const sdkws::GroupInfo &group)
{
    try
    {
        auto stmt = boost::mysql::with_params(
            "INSERT INTO groups (group_id, group_name, notification, introduction, "
            "face_url, owner_user_id, creator_user_id, create_time, member_count, "
            "status, group_type, need_verification, look_member_info, "
            "apply_member_friend, notification_update_time, notification_user_id, ex) "
            "VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {})",
            group.groupid(),
            group.groupname(),
            group.notification(),
            group.introduction(),
            group.faceurl(),
            group.owneruserid(),
            group.creatoruserid(),
            group.createtime(),
            static_cast<int64_t>(group.membercount()),
            static_cast<int64_t>(group.status()),
            static_cast<int64_t>(group.grouptype()),
            static_cast<int64_t>(group.needverification()),
            static_cast<int64_t>(group.lookmemberinfo()),
            static_cast<int64_t>(group.applymemberfriend()),
            group.notificationupdatetime(),
            group.notificationuserid(),
            group.ex());

        co_await MySQLConnector::instance().async_execute(stmt);
        co_return true;
    }
    catch (const std::exception &e)
    {
        spdlog::error("CreateGroup failed: {}", e.what());
        co_return false;
    }
}

// ── 查询群信息 ──

boost::asio::awaitable<std::optional<sdkws::GroupInfo>>
MySQLHandler::GetGroupInfo(const std::string &group_id)
{
    try
    {
        auto stmt = boost::mysql::with_params(
            "SELECT group_id, group_name, notification, introduction, face_url, "
            "owner_user_id, creator_user_id, create_time, member_count, status, "
            "group_type, need_verification, look_member_info, apply_member_friend, "
            "notification_update_time, notification_user_id, ex "
            "FROM groups WHERE group_id = {}",
            group_id);

        boost::mysql::results result = co_await MySQLConnector::instance().async_execute(stmt);
        auto rows = result.rows();
        if (rows.empty())
        {
            co_return std::nullopt;
        }
        co_return parse_group_from_row(rows);
    }
    catch (const std::exception &e)
    {
        spdlog::error("GetGroupInfo failed: {}", e.what());
        co_return std::nullopt;
    }
}

// ── 更新群信息 ──

boost::asio::awaitable<bool>
MySQLHandler::UpdateGroupInfo(const std::string &group_id,
                               const sdkws::GroupInfoForSet &update)
{
    try
    {
        auto stmt = boost::mysql::with_params(
            "UPDATE groups SET "
            "group_name = COALESCE(NULLIF({}, ''), group_name), "
            "notification = COALESCE(NULLIF({}, ''), notification), "
            "introduction = COALESCE(NULLIF({}, ''), introduction), "
            "face_url = COALESCE(NULLIF({}, ''), face_url), "
            "need_verification = {}, "
            "look_member_info = {}, "
            "apply_member_friend = {} "
            "WHERE group_id = {}",
            update.groupname(),
            update.notification(),
            update.introduction(),
            update.faceurl(),
            static_cast<int64_t>(update.needverification()),
            static_cast<int64_t>(update.lookmemberinfo()),
            static_cast<int64_t>(update.applymemberfriend()),
            group_id);

        co_await MySQLConnector::instance().async_execute(stmt);
        co_return true;
    }
    catch (const std::exception &e)
    {
        spdlog::error("UpdateGroupInfo failed: {}", e.what());
        co_return false;
    }
}

// ── 解散群 ──

boost::asio::awaitable<bool>
MySQLHandler::DismissGroup(const std::string &group_id)
{
    try
    {
        auto stmt = boost::mysql::with_params(
            "UPDATE groups SET status = 1 WHERE group_id = {}", group_id);

        co_await MySQLConnector::instance().async_execute(stmt);
        co_return true;
    }
    catch (const std::exception &e)
    {
        spdlog::error("DismissGroup failed: {}", e.what());
        co_return false;
    }
}

// ── 添加群成员（批量，不开启事务，调用者在外层用事务包装） ──

boost::asio::awaitable<bool>
MySQLHandler::AddGroupMembers(const std::string &group_id,
                               const std::vector<sdkws::GroupMemberFullInfo> &members)
{
    if (members.empty())
    {
        co_return true;
    }

    try
    {
        // 批量 INSERT：逐个执行（MySQL 参数化暂不支持多行 VALUES）
        for (const auto &m : members)
        {
            auto stmt = boost::mysql::with_params(
                "INSERT INTO group_members (group_id, user_id, role_level, "
                "join_time, nickname, face_url, join_source, operator_user_id, "
                "inviter_user_id, mute_end_time, ex) "
                "VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {})",
                group_id,
                m.userid(),
                static_cast<int64_t>(m.rolelevel()),
                m.jointime(),
                m.nickname(),
                m.faceurl(),
                static_cast<int64_t>(m.joinsource()),
                m.operatoruserid(),
                m.inviteruserid(),
                m.muteendtime(),
                m.ex());

            co_await MySQLConnector::instance().async_execute(stmt);
        }
        co_return true;
    }
    catch (const std::exception &e)
    {
        spdlog::error("AddGroupMembers failed: {}", e.what());
        co_return false;
    }
}

// ── 移除群成员 ──

boost::asio::awaitable<bool>
MySQLHandler::RemoveGroupMembers(const std::string &group_id,
                                  const std::vector<std::string> &user_ids)
{
    if (user_ids.empty())
    {
        co_return true;
    }

    try
    {
        // 逐个删除
        for (const auto &uid : user_ids)
        {
            auto stmt = boost::mysql::with_params(
                "DELETE FROM group_members WHERE group_id = {} AND user_id = {}",
                group_id, uid);

            co_await MySQLConnector::instance().async_execute(stmt);
        }
        co_return true;
    }
    catch (const std::exception &e)
    {
        spdlog::error("RemoveGroupMembers failed: {}", e.what());
        co_return false;
    }
}

// ── 更新群成员数 ──

boost::asio::awaitable<bool>
MySQLHandler::UpdateGroupMemberCount(const std::string &group_id, int32_t delta)
{
    try
    {
        auto stmt = boost::mysql::with_params(
            "UPDATE groups SET member_count = member_count + {} WHERE group_id = {}",
            static_cast<int64_t>(delta), group_id);

        co_await MySQLConnector::instance().async_execute(stmt);
        co_return true;
    }
    catch (const std::exception &e)
    {
        spdlog::error("UpdateGroupMemberCount failed: {}", e.what());
        co_return false;
    }
}

// ── 查询群成员列表 ──

boost::asio::awaitable<std::vector<sdkws::GroupMemberFullInfo>>
MySQLHandler::GetGroupMembers(const std::string &group_id, int offset, int limit)
{
    std::vector<sdkws::GroupMemberFullInfo> members;
    try
    {
        auto stmt = boost::mysql::with_params(
            "SELECT group_id, user_id, role_level, join_time, nickname, face_url, "
            "join_source, operator_user_id, inviter_user_id, mute_end_time, ex "
            "FROM group_members WHERE group_id = {} "
            "ORDER BY join_time ASC LIMIT {} OFFSET {}",
            group_id, limit, offset);

        boost::mysql::results result = co_await MySQLConnector::instance().async_execute(stmt);
        auto rows = result.rows();
        for (size_t i = 0; i < rows.size(); ++i)
        {
            members.push_back(parse_member_from_row(rows, i));
        }
    }
    catch (const std::exception &e)
    {
        spdlog::error("GetGroupMembers failed: {}", e.what());
    }
    co_return members;
}

// ── 查询群成员数 ──

boost::asio::awaitable<int64_t>
MySQLHandler::GetGroupMemberCount(const std::string &group_id)
{
    try
    {
        auto stmt = boost::mysql::with_params(
            "SELECT COUNT(*) FROM group_members WHERE group_id = {}", group_id);

        boost::mysql::results result = co_await MySQLConnector::instance().async_execute(stmt);
        auto rows = result.rows();
        if (!rows.empty())
        {
            co_return rows.at(0).at(0).as_int64();
        }
    }
    catch (const std::exception &e)
    {
        spdlog::error("GetGroupMemberCount failed: {}", e.what());
    }
    co_return 0;
}

// ── 查询用户所在的群 ID 列表 ──

boost::asio::awaitable<std::vector<std::string>>
MySQLHandler::GetUserGroupIDs(const std::string &user_id)
{
    std::vector<std::string> group_ids;
    try
    {
        auto stmt = boost::mysql::with_params(
            "SELECT group_id FROM group_members WHERE user_id = {}", user_id);

        boost::mysql::results result = co_await MySQLConnector::instance().async_execute(stmt);
        auto rows = result.rows();
        for (size_t i = 0; i < rows.size(); ++i)
        {
            group_ids.push_back(rows.at(i).at(0).as_string());
        }
    }
    catch (const std::exception &e)
    {
        spdlog::error("GetUserGroupIDs failed: {}", e.what());
    }
    co_return group_ids;
}

// ── 判断用户是否在群内 ──

boost::asio::awaitable<bool>
MySQLHandler::IsUserInGroup(const std::string &group_id, const std::string &user_id)
{
    try
    {
        auto stmt = boost::mysql::with_params(
            "SELECT 1 FROM group_members WHERE group_id = {} AND user_id = {}",
            group_id, user_id);

        boost::mysql::results result = co_await MySQLConnector::instance().async_execute(stmt);
        co_return !result.rows().empty();
    }
    catch (const std::exception &e)
    {
        spdlog::error("IsUserInGroup failed: {}", e.what());
        co_return false;
    }
}