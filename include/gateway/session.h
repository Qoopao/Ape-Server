#pragma once
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <memory>
#include <user/userinfo.h>
#include <atomic>

class Session : public std::enable_shared_from_this<Session>
{
public:
    Session(boost::asio::ip::tcp::socket socket);
    boost::asio::awaitable<void> run();

    userInfo getuser();
    bool validate_auth(std::string userid);

private:
    boost::asio::awaitable<void> do_read();
    void process_message(const std::string &message);
    boost::asio::awaitable<void> do_write(const std::string &message);

    boost::beast::websocket::stream<boost::asio::ip::tcp::socket> ws_;
    userInfo user;
    std::atomic<bool> is_authenticated{false};
};
