#pragma once
#include <boost/asio.hpp>
#include <boost/beast.hpp>

class WebServer
{
public:
    WebServer(boost::asio::io_context &ioc, uint16_t port);

    boost::asio::awaitable<void> run();

    void stop();

private:
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::io_context &ioc_;            
    boost::asio::awaitable<void> do_accept();
};