#ifndef USERINFO_H
#define USERINFO_H

#include <string>

struct userInfo {
    std::string user_id;
    std::string username;
    std::string nickname;
    std::string password_hash;
    std::string password_salt;
};

#endif
