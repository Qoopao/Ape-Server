#ifndef MYSQLHANDLER_H
#define MYSQLHANDLER_H

#include "user/userinfo.h"

#include <boost/asio/awaitable.hpp>
#include <optional>
#include <string>

// ============================================================================
// MySQLHandler — MySQL 异步操作封装
//
// 所有方法均为协程方法（boost::asio::awaitable<T>），
// 内部通过 co_await MySQLConnector::async_execute() 异步执行 SQL。
// ============================================================================

class MySQLHandler
{
public:
    // 插入用户，返回插入后的 userInfo（失败返回 nullopt）
    static boost::asio::awaitable<std::optional<userInfo>>
        insert_user(const userInfo &user);

    // 通过用户名查找用户
    static boost::asio::awaitable<std::optional<userInfo>>
        find_user_by_username(const std::string &username);

    // 通过 user_id 查找用户
    static boost::asio::awaitable<std::optional<userInfo>>
        find_user_by_id(const std::string &user_id);

    // 更新最后登录时间
    static boost::asio::awaitable<bool>
        update_last_login(const std::string &user_id);

    // 检查用户名是否已存在
    static boost::asio::awaitable<bool>
        username_exists(const std::string &username);

    // 获取用户在线信息（用于 backbon 服务，包含 last_login_at）
    static boost::asio::awaitable<std::optional<userInfo>>
        get_user_online_info(const std::string &user_id);
};

#endif