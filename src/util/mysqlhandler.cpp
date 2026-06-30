#include "util/mysqlhandler.h"
#include "util/mysqlconnector.h"

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