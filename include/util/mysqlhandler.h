#ifndef MYSQLHANDLER_H
#define MYSQLHANDLER_H

#include "user/userinfo.h"
#include <optional>
#include <string>

class MySQLHandler
{
public:
    static std::optional<userInfo> insert_user(const userInfo &user);
    static std::optional<userInfo> find_user_by_username(const std::string &username);
    static std::optional<userInfo> find_user_by_id(const std::string &user_id);
    static bool update_last_login(const std::string &user_id);

    // 检查用户名是否已存在
    static bool username_exists(const std::string &username);
};

#endif