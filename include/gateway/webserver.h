#ifndef WEBSERVER_H
#define WEBSERVER_H



#include <boost/asio.hpp>
#include <boost/beast.hpp>

using boost::asio::ip::tcp;
using boost::asio::awaitable;
using boost::asio::co_spawn;
using boost::asio::detached;
using boost::asio::use_awaitable;
namespace this_coro = boost::asio::this_coro;

class WebServer
{
public:
    WebServer(boost::asio::io_context &ioc, uint16_t port);

    boost::asio::awaitable<void> listener();

    void stop();

private:
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::io_context &ioc_;            
    boost::asio::awaitable<void> do_accept();
};

#endif