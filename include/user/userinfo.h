#ifndef USERINFO_H
#define USERINFO_H

#include <string>
#include <sys/types.h>

struct userInfo {
    u_int64_t user_id;
    u_int64_t account;
    std::string nickname;
    std::string phone;
    std::string password_hash;
    std::string password_salt;
    std::string created_at;
    std::string updated_at;
    std::string last_login_at;
};

#endif
