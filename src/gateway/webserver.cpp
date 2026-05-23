#include <boost/asio.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/this_coro.hpp> // 确保包含this_coro头文件
#include <boost/asio/thread_pool.hpp>
#include <boost/beast.hpp>
#include <spdlog/spdlog.h>

// 简化命名空间，提升代码可读性
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using boost::asio::awaitable;
using boost::asio::co_spawn;
using boost::asio::detached;
using boost::asio::use_awaitable;

#include <grpcpp/grpcpp.h>

#include "gateway/webserver.h"
#include "gateway/ws_session.h"
#include "services/auth_service/client.h"
#include "services/backbon_service/client.h"
#include "services/msg_service/client.h"
#include "services/push_service/client.h"

void WebServer::discover_Service(std::string service_name,
                                 std::string &service_addr) {
  try {
    auto resp = backbon_client_->GetServicesList(service_name);
    if (resp.registered() && resp.ipport_size() > 0) {
      service_addr = resp.ipport(0);
      spdlog::info("WebServer: discovered {} at {}", service_name,
                   service_addr);
    } else {
      spdlog::warn(
          "WebServer: {} not found in etcd, using fallback {}",
          service_name, service_addr);
    }
  } catch (const std::exception &e) {
    spdlog::error("WebServer: failed to discover {} : {}, using fallback",
                  service_name, e.what());
  }
}

WebServer::WebServer(int ioc_pool_size, uint16_t port,
                     std::unique_ptr<BackbonClient> backbon_client)
    : _iocPool(ioc_pool_size), _acceptor_ioc(1),
      acceptor_(_acceptor_ioc, tcp::endpoint(tcp::v4(), port)),
      backbon_client_(std::move(backbon_client)) {

  // backbon、gateway_push写死地址，其余服务需要远程获取
  // 通过 BackbonService (etcd) 服务发现获取 AuthService 地址
  std::string auth_addr = "localhost:50051"; // fallback
  std::string msg_addr = "localhost:50053";  // fallback
  std::string push_addr = "localhost:50054"; // fallback

  discover_Service("AuthService", auth_addr);
  discover_Service("MsgService", msg_addr);
  discover_Service("PushService", push_addr);

  auth_channel_ =
      grpc::CreateChannel(auth_addr, grpc::InsecureChannelCredentials());
  auth_client_ = std::make_unique<AuthClient>(auth_channel_);
  msg_channel_ =
      grpc::CreateChannel(msg_addr, grpc::InsecureChannelCredentials());
  msg_client_ = std::make_unique<MsgClient>(msg_channel_);
  push_channel_ =
      grpc::CreateChannel(push_addr, grpc::InsecureChannelCredentials());
  push_client_ = std::make_unique<PushClient>(push_channel_);

  spdlog::info("WebSocketServer initialized, listening on port {}", port);
}

void WebServer::start() {
  
  // 注册与监听 SIGINT 和 SIGTERM 信号
  boost::asio::signal_set signals(get_acceptor_ioc(), SIGINT, SIGTERM);
  signals.async_wait([&](auto, auto) {stop(); });

  // 启动IOCPool与accept_ioc
  _iocPool.startPool();
  boost::asio::co_spawn(get_acceptor_ioc(), listener(),
                        boost::asio::detached);
  get_acceptor_ioc().run();
}

// 停止服务器
void WebServer::stop() {
  _iocPool.stopPool();
  get_acceptor_ioc().stop();
  acceptor_.close();
}

// 处理单个 WebSocket 连接：接受 TCP 后升级为 WebSocket，然后交给 WSSession 管理
awaitable<void> handle_ws_session(tcp::socket socket, AuthClient *auth_client,
                                  MsgClient *msg_client,
                                  PushClient *push_client) {
  auto session = std::make_shared<WSSession>(std::move(socket), auth_client,
                                             msg_client, push_client);
  co_await session->start();
}

// 先使用一个ioc进行监听，将接收到的连接投递到iocPool进行处理
asio::awaitable<void> WebServer::listener() {

  for (;;) {
    try {
      // 异步接受连接
      tcp::socket socket = co_await acceptor_.async_accept(use_awaitable);
      // 启动协程处理 WebSocket 连接（用ioc_作为executor，更直观）
      _iocPool.spawn(handle_ws_session(std::move(socket), auth_client_.get(),
                                       msg_client_.get(), push_client_.get()));
    } catch (const std::exception &e) {
      spdlog::warn("Accept error: {}", e.what());
      // 仅当acceptor关闭时退出循环
      if (!acceptor_.is_open()) {
        spdlog::info("Acceptor closed, exiting listener loop");
        break;
      }
      // 临时错误（如网络波动），继续接受连接
    }
  }
  co_return; // 显式返回，符合协程规范
}