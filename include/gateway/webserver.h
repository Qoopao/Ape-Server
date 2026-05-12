#ifndef WEBSERVER_H
#define WEBSERVER_H



#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <grpcpp/channel.h>
#include <memory>

class AuthClient;
class MsgClient;
class BackbonClient;

using boost::asio::ip::tcp;
using boost::asio::awaitable;
using boost::asio::co_spawn;
using boost::asio::detached;
using boost::asio::use_awaitable;
namespace this_coro = boost::asio::this_coro;

class WebServer
{
public:
    WebServer(boost::asio::io_context &ioc, uint16_t port,
              std::unique_ptr<BackbonClient> backbon_client);
    ~WebServer();

    boost::asio::awaitable<void> listener();

    void stop();

    // 供 WSSession 获取 AuthClient
    AuthClient* getAuthClient() { return auth_client_.get(); }

    // 供 WSSession 获取 MsgClient
    MsgClient* getMsgClient() { return msg_client_.get(); }

private:
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::io_context &ioc_;
    boost::asio::awaitable<void> do_accept();
    std::shared_ptr<grpc::Channel> auth_channel_;
    std::unique_ptr<AuthClient> auth_client_;
    std::shared_ptr<grpc::Channel> msg_channel_;
    std::unique_ptr<MsgClient> msg_client_;
    std::unique_ptr<BackbonClient> backbon_client_;
};

#endif