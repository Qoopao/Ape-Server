#include "util/mysqlhandler.h"
#include <boost/asio/io_context.hpp>
#include <boost/mysql.hpp>
#include <spdlog/spdlog.h>
#include <memory>

namespace {

// 辅助：同步方式执行 MySQL 查询
// 每次调用创建独立的 io_context + any_connection
template <typename Func>
auto run_mysql_sync(Func &&func)
{
    boost::asio::io_context ioc;

    boost::mysql::connect_params params;
    params.server_address.emplace_host_and_port("127.0.0.1", 3306);
    params.username = "ape_user";
    params.password = "ape_password";
    params.database = "ape_auth";
    params.ssl = boost::mysql::ssl_mode::disable;

    auto conn = std::make_shared<boost::mysql::any_connection>(ioc);

    // 建立连接
    boost::mysql::diagnostics diag;
    boost::mysql::error_code connect_ec;
    conn->async_connect(params, diag,
                        [&connect_ec](boost::mysql::error_code ec) { connect_ec = ec; });
    ioc.run();
    ioc.restart();

    if (connect_ec)
    {
        spdlog::error("MySQL connect error: {}", connect_ec.message());
        return decltype(func(conn, ioc)){};
    }

    // 执行业务逻辑
    auto result = func(conn, ioc);

    // 关闭连接
    boost::mysql::error_code close_ec;
    conn->async_close(diag, [&close_ec](boost::mysql::error_code ec) { close_ec = ec; });
    ioc.run();

    if (close_ec)
    {
        spdlog::warn("MySQL close warning: {}", close_ec.message());
    }

    return result;
}

} // namespace

std::optional<userInfo> MySQLHandler::insert_user(const userInfo &user)
{
    return run_mysql_sync([&](auto conn, auto &ioc) -> std::optional<userInfo> {
        try
        {
            boost::mysql::results result;
            boost::mysql::diagnostics diag;
            boost::mysql::error_code ec;

            // 使用 with_params 做参数化插入
            auto stmt = boost::mysql::with_params(
                "INSERT INTO users (user_id, username, nickname, password_hash, password_salt) "
                "VALUES ({}, {}, {}, {}, {})",
                user.user_id,
                user.username,
                user.nickname,
                user.password_hash,
                user.password_salt);

            conn->async_execute(std::move(stmt), result, diag,
                                [&ec](boost::mysql::error_code e) { ec = e; });
            ioc.run();
            ioc.restart();

            if (ec)
            {
                spdlog::error("Insert user failed: {}", ec.message());
                return std::nullopt;
            }

            spdlog::info("Inserted user: {} ({})", user.username, user.user_id);
            return user;
        }
        catch (const std::exception &e)
        {
            spdlog::error("Insert user exception: {}", e.what());
            return std::nullopt;
        }
    });
}

std::optional<userInfo> MySQLHandler::find_user_by_username(const std::string &username)
{
    return run_mysql_sync([&](auto conn, auto &ioc) -> std::optional<userInfo> {
        try
        {
            boost::mysql::results result;
            boost::mysql::diagnostics diag;
            boost::mysql::error_code ec;

            auto stmt = boost::mysql::with_params(
                "SELECT user_id, username, nickname, password_hash, password_salt, "
                "created_at, updated_at, last_login_at FROM users WHERE username = {}",
                username);

            conn->async_execute(std::move(stmt), result, diag,
                                [&ec](boost::mysql::error_code e) { ec = e; });
            ioc.run();
            ioc.restart();

            if (ec)
            {
                spdlog::error("Find user by username failed: {}", ec.message());
                return std::nullopt;
            }

            auto rows = result.rows();
            if (rows.empty())
            {
                spdlog::warn("User '{}' not found", username);
                return std::nullopt;
            }

            auto row = rows[0];
            userInfo user;
            user.user_id = row.at(0).as_string();
            user.username = row.at(1).as_string();
            user.nickname = row.at(2).as_string();
            user.password_hash = row.at(3).as_string();
            user.password_salt = row.at(4).as_string();

            return user;
        }
        catch (const std::exception &e)
        {
            spdlog::error("Find user by username exception: {}", e.what());
            return std::nullopt;
        }
    });
}

std::optional<userInfo> MySQLHandler::find_user_by_id(const std::string &user_id)
{
    return run_mysql_sync([&](auto conn, auto &ioc) -> std::optional<userInfo> {
        try
        {
            boost::mysql::results result;
            boost::mysql::diagnostics diag;
            boost::mysql::error_code ec;

            auto stmt = boost::mysql::with_params(
                "SELECT user_id, username, nickname, password_hash, password_salt, "
                "created_at, updated_at, last_login_at FROM users WHERE user_id = {}",
                user_id);

            conn->async_execute(std::move(stmt), result, diag,
                                [&ec](boost::mysql::error_code e) { ec = e; });
            ioc.run();
            ioc.restart();

            if (ec)
            {
                spdlog::error("Find user by id failed: {}", ec.message());
                return std::nullopt;
            }

            auto rows = result.rows();
            if (rows.empty())
            {
                spdlog::warn("User '{}' not found", user_id);
                return std::nullopt;
            }

            auto row = rows[0];
            userInfo user;
            user.user_id = row.at(0).as_string();
            user.username = row.at(1).as_string();
            user.nickname = row.at(2).as_string();
            user.password_hash = row.at(3).as_string();
            user.password_salt = row.at(4).as_string();

            return user;
        }
        catch (const std::exception &e)
        {
            spdlog::error("Find user by id exception: {}", e.what());
            return std::nullopt;
        }
    });
}

bool MySQLHandler::update_last_login(const std::string &user_id)
{
    return run_mysql_sync([&](auto conn, auto &ioc) -> bool {
        try
        {
            boost::mysql::results result;
            boost::mysql::diagnostics diag;
            boost::mysql::error_code ec;

            auto stmt = boost::mysql::with_params(
                "UPDATE users SET last_login_at = NOW() WHERE user_id = {}", user_id);

            conn->async_execute(std::move(stmt), result, diag,
                                [&ec](boost::mysql::error_code e) { ec = e; });
            ioc.run();

            if (ec)
            {
                spdlog::error("Update last_login failed: {}", ec.message());
                return false;
            }

            return true;
        }
        catch (const std::exception &e)
        {
            spdlog::error("Update last_login exception: {}", e.what());
            return false;
        }
    });
}

bool MySQLHandler::username_exists(const std::string &username)
{
    return run_mysql_sync([&](auto conn, auto &ioc) -> bool {
        try
        {
            boost::mysql::results result;
            boost::mysql::diagnostics diag;
            boost::mysql::error_code ec;

            auto stmt = boost::mysql::with_params(
                "SELECT COUNT(*) as cnt FROM users WHERE username = {}", username);

            conn->async_execute(std::move(stmt), result, diag,
                                [&ec](boost::mysql::error_code e) { ec = e; });
            ioc.run();
            ioc.restart();

            if (ec)
            {
                spdlog::error("Check username exists failed: {}", ec.message());
                return false;
            }

            auto rows = result.rows();
            if (!rows.empty())
            {
                int64_t count = rows[0].at(0).as_int64();
                return count > 0;
            }
            return false;
        }
        catch (const std::exception &e)
        {
            spdlog::error("Check username exists exception: {}", e.what());
            return false;
        }
    });
}