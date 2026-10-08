#pragma once

#include "user/userinfo.h"
#include <boost/asio/awaitable.hpp>
#include <boost/mysql/connection_pool.hpp>
#include <stdexcept>
#include <string>

namespace registration {
struct Error : std::runtime_error {
    std::string code;
    Error(std::string code, std::string message)
        : std::runtime_error(std::move(message)), code(std::move(code)) {}
};
struct Input {
    std::string request_id;
    userInfo user;
    // Length-prefixed original input; kept in memory only, never logged/stored.
    std::string payload;
};
struct Result { uint64_t user_id; uint64_t account; };
std::string payload(const std::string& phone, const std::string& nickname,
                    const std::string& password);
boost::asio::awaitable<Result> create(boost::mysql::connection_pool& pool, const Input& input);
boost::asio::awaitable<void> refill(boost::mysql::connection_pool& pool, uint64_t batch_size);
} // namespace registration