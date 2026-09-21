#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast.hpp>
#include <boost/asio/ssl.hpp>

#include <climits>
#include <cstdint>
#include <grpcpp/channel.h>
#include <memory>

#include "util/ioc_pool.h"

class AuthClient;
class MsgClient;
class BackbonClient;
class PushClient;
class GroupClient;

using boost::asio::awaitable;
using boost::asio::co_spawn;
using boost::asio::detached;
using boost::asio::use_awaitable;
using boost::asio::ip::tcp;

class WebServer {

public:
  WebServer(int ioc_pool_size, uint16_t port,
            std::unique_ptr<BackbonClient> backbon_client);
  ~WebServer();

  boost::asio::awaitable<void> listener();

  void start();
  void stop();

  boost::asio::awaitable<std::string>
  discover_Service(const std::string &service_name,
                   const std::string &fallback);

  boost::asio::io_context &get_acceptor_ioc() { return _acceptor_ioc; }

  // 供 WSSession 获取 AuthClient、MsgClient、PushClient
  AuthClient *getAuthClient() { return auth_client_.get(); }
  MsgClient *getMsgClient() { return msg_client_.get(); }
  PushClient *getPushClient() { return push_client_.get(); }
  GroupClient *getGroupClient() { return group_client_.get(); }

  // 异步初始化：发现所有下游服务并启动监听循环
  boost::asio::awaitable<void> init_and_listen();

private:
  boost::asio::ssl::context ssl_ctx_{boost::asio::ssl::context::tls_server};
  IOC_Pool _iocPool;
  boost::asio::io_context _acceptor_ioc;
  boost::asio::ip::tcp::acceptor acceptor_;

  std::shared_ptr<grpc::Channel> auth_channel_;
  std::unique_ptr<AuthClient> auth_client_;
  std::shared_ptr<grpc::Channel> msg_channel_;
  std::unique_ptr<MsgClient> msg_client_;
  std::shared_ptr<grpc::Channel> push_channel_;
  std::unique_ptr<PushClient> push_client_;
  std::shared_ptr<grpc::Channel> group_channel_;
  std::unique_ptr<GroupClient> group_client_;
  std::unique_ptr<BackbonClient> backbon_client_;
};

#endif