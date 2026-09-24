#include "storage/mysqlhandler.h"
#include "storage/mysqlconnector.h"
#include "sdkws.pb.h"

#include <boost/asio/use_awaitable.hpp>
#include <boost/mysql/error_with_diagnostics.hpp>
#include <boost/mysql/results.hpp>
#include <boost/mysql/with_params.hpp>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <spdlog/spdlog.h>

// ═══════════════════════════════════════════════════════════════════════════
// 辅助：将 results.rows() 的第一行解析为 userInfo
// 字段顺序：user_id, account, nickname, password_hash, password_salt
// ═══════════════════════════════════════════════════════════════════════════
namespace {

userInfo parse_user_from_row(const boost::mysql::rows_view &rows)
{
    auto row = rows.at(0);
    userInfo user;

    user.user_id       = row.at(0).as_uint64();   // user_id
    user.account       = row.at(1).as_uint64();   // account
    user.nickname      = row.at(2).as_string();   // nickname
    user.phone         = row.at(3).as_string();   // phone
    user.password_hash = row.at(4).as_string();   // password_hash
    user.password_salt = row.at(5).as_string();   // password_salt

    // created_at → 第 7 列 (index 6)
    if (row.size() > 6 && !row.at(6).is_null())
    {
        auto dt = row.at(6).as_datetime();
        std::ostringstream oss;
        oss << static_cast<int>(dt.year()) << '-'
            << std::setw(2) << std::setfill('0') << static_cast<int>(dt.month()) << '-'
            << std::setw(2) << std::setfill('0') << static_cast<int>(dt.day()) << ' '
            << std::setw(2) << std::setfill('0') << static_cast<int>(dt.hour()) << ':'
            << std::setw(2) << std::setfill('0') << static_cast<int>(dt.minute()) << ':'
            << std::setw(2) << std::setfill('0') << static_cast<int>(dt.second());
        user.created_at = oss.str();
    }

    // updated_at → 第 8 列 (index 7)
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
        user.updated_at = oss.str();
    }

    // last_login_at → 第 9 列 (index 8)，可为 NULL
    if (row.size() > 8 && !row.at(8).is_null())
    {
        auto dt = row.at(8).as_datetime();
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
boost::asio::awaitable<std::optional<uint64_t>>
MySQLHandler::insert_user(const userInfo &user)
{
    try
    {
        // account 暂为 0（表里 DEFAULT 0），等取号后再 UPDATE
        // phone 必填，从 user.phone 传入
        auto stmt = boost::mysql::with_params(
            "INSERT INTO users (account, nickname, phone, password_hash, password_salt) "
            "VALUES (0, {}, {}, {}, {})",
            user.nickname,
            user.phone,
            user.password_hash,
            user.password_salt);

        boost::mysql::results result =
            co_await MySQLConnector::instance().async_execute(stmt);

        uint64_t id = result.last_insert_id();
        spdlog::info("[MySQL] Inserted user: user_id={}", id);
        co_return id;
    }
    catch (const std::exception &e)
    {
        spdlog::error("[MySQL] insert_user failed: {}", e.what());
        co_return std::nullopt;
    }
}

boost::asio::awaitable<bool>
MySQLHandler::update_account(uint64_t user_id, uint64_t account)
{
    try
    {
        auto stmt = boost::mysql::with_params(
            "UPDATE users SET account = {} WHERE user_id = {}",
            account, user_id);

        co_await MySQLConnector::instance().async_execute(stmt);

        // async_execute 返回受影响行数，为 1 表示更新成功
        co_return true;
    }
    catch (const std::exception &e)
    {
        spdlog::error(
            "update_account failed: user_id={}, account={}, err={}",
            user_id, account, e.what());
        co_return false;
    }
}

// ── 通过账号查找用户 ──

boost::asio::awaitable<std::optional<userInfo>>
MySQLHandler::find_user_by_account(const uint64_t account)
{
    try
    {
        auto stmt = boost::mysql::with_params(
            "SELECT user_id, account, nickname, phone, password_hash, password_salt, "
            "created_at, updated_at, last_login_at FROM users WHERE account = {}",
            account);

        boost::mysql::results result =
            co_await MySQLConnector::instance().async_execute(stmt);
        auto rows = result.rows();
        if (rows.empty())
        {
            spdlog::warn("User '{}' not found", account);
            co_return std::nullopt;
        }

        co_return parse_user_from_row(rows);
    }
    catch (const std::exception &e)
    {
        spdlog::error("Find user by account failed: {}", e.what());
        co_return std::nullopt;
    }
}

// ── 通过 user_id 查找用户 ──

boost::asio::awaitable<std::optional<userInfo>>
MySQLHandler::find_user_by_id(const uint64_t user_id)
{
    try
    {
        auto stmt = boost::mysql::with_params(
            "SELECT user_id, account, nickname, phone, password_hash, password_salt, "
            "created_at, updated_at, last_login_at FROM users WHERE user_id = {}",
            user_id);

        boost::mysql::results result =
            co_await MySQLConnector::instance().async_execute(stmt);
        auto rows = result.rows();
        if (rows.empty())
        {
            spdlog::warn("User '{}' not found", user_id);
            co_return std::nullopt;
        }

        co_return parse_user_from_row(rows);
    }
    catch (const std::exception &e)
    {
        spdlog::error("Find user by id failed: {}", e.what());
        co_return std::nullopt;
    }
}

// ── 更新最后登录时间 ──

boost::asio::awaitable<bool>
MySQLHandler::update_last_login(const uint64_t user_id)
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
MySQLHandler::account_exists(const uint64_t account)
{
    try
    {
        auto stmt = boost::mysql::with_params(
            "SELECT COUNT(*) FROM users WHERE account = {}", account);

        boost::mysql::results result =
            co_await MySQLConnector::instance().async_execute(stmt);
        auto rows = result.rows();
        if (!rows.empty())
        {
            co_return rows.at(0).at(0).as_int64() > 0;
        }
        co_return false;
    }
    catch (const std::exception &e)
    {
        spdlog::error("Check account exists failed: {}", e.what());
        co_return false;
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
    g.set_owneruserid(row.at(5).as_uint64());
    g.set_creatoruserid(row.at(6).as_uint64());
    g.set_createtime(row.at(7).as_int64());
    g.set_membercount(static_cast<uint32_t>(row.at(8).as_int64()));
    g.set_status(row.at(9).as_int64());
    g.set_grouptype(row.at(10).as_int64());
    g.set_needverification(row.at(11).as_int64());
    g.set_lookmemberinfo(row.at(12).as_int64());
    g.set_applymemberfriend(row.at(13).as_int64());
    if (!row.at(14).is_null()) g.set_notificationupdatetime(row.at(14).as_int64());
    if (!row.at(15).is_null()) g.set_notificationuserid(row.at(15).as_uint64());
    if (!row.at(16).is_null()) g.set_ex(std::string(row.at(16).as_string()));
    return g;
}

sdkws::GroupMemberFullInfo parse_member_from_row(const boost::mysql::rows_view &rows, size_t idx)
{
    auto row = rows.at(idx);
    sdkws::GroupMemberFullInfo m;
    m.set_groupid(std::string(row.at(0).as_string()));
    m.set_userid(row.at(1).as_uint64());
    m.set_rolelevel(row.at(2).as_int64());
    m.set_jointime(row.at(3).as_int64());
    if (!row.at(4).is_null()) m.set_nickname(std::string(row.at(4).as_string()));
    if (!row.at(5).is_null()) m.set_faceurl(std::string(row.at(5).as_string()));
    m.set_joinsource(row.at(6).as_int64());
    if (!row.at(7).is_null()) m.set_operatoruserid(row.at(7).as_uint64());
    if (!row.at(8).is_null()) m.set_inviteruserid(row.at(8).as_uint64());
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
            static_cast<int32_t>(group.membercount()),
            static_cast<int32_t>(group.status()),
            static_cast<int32_t>(group.grouptype()),
            static_cast<int32_t>(group.needverification()),
            static_cast<int32_t>(group.lookmemberinfo()),
            static_cast<int32_t>(group.applymemberfriend()),
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
            static_cast<int32_t>(update.needverification()),
            static_cast<int32_t>(update.lookmemberinfo()),
            static_cast<int32_t>(update.applymemberfriend()),
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
                static_cast<int32_t>(m.rolelevel()),
                m.jointime(),
                m.nickname(),
                m.faceurl(),
                static_cast<int32_t>(m.joinsource()),
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
                                  const std::vector<uint64_t> &user_ids)
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
            delta, group_id);

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
MySQLHandler::GetUserGroupIDs(const uint64_t user_id)
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
MySQLHandler::IsUserInGroup(const std::string &group_id, const uint64_t user_id)
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