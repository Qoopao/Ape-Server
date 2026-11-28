#include <boost/asio.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/beast.hpp>
#include <spdlog/spdlog.h>

// 简化命名空间
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using boost::asio::awaitable;
using boost::asio::co_spawn;
using boost::asio::detached;
using boost::asio::use_awaitable;

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "gateway/webserver.h"
#include "gateway/ws_session.h"
#include "gateway/ws_session_manager.h"
#include "services/auth_service/client.h"
#include "services/backbon_service/client.h"
#include "services/conversation_service/client.h"
#include "services/friend_service/client.h"
#include "services/gateway_router.h"
#include "services/group_service/client.h"
#include "services/msg_service/client.h"
#include "services/push_service/client.h"

namespace {

int DrainGraceSeconds() {
  const char *value = std::getenv("GATEWAY_DRAIN_GRACE_SECONDS");
  if (value == nullptr)
    return 30;
  try {
    return std::max(0, std::stoi(value));
  } catch (const std::exception &) {
    return 30;
  }
}

} // namespace

boost::asio::awaitable<std::string> WebServer::discover_Service(const std::string& service_name,
                                                                 const std::string& fallback) {
  try {
    auto resp = co_await backbon_client_->GetServicesList(service_name);
    if (resp.registered() && resp.instances_size() > 0) {
      const std::string addr = resp.instances(0).address();
      spdlog::info("WebServer: discovered {} at {}", service_name, addr);
      co_return addr;
    }
    spdlog::warn("WebServer: {} not registered in etcd, using fallback {}", service_name, fallback);
  } catch (const std::exception &e) {
    spdlog::error("WebServer: failed to discover {} : {}, using fallback {}", service_name,
                  e.what(), fallback);
  }
  co_return fallback;
}

WebServer::WebServer(int ioc_pool_size, uint16_t port,
                     std::unique_ptr<BackbonClient> backbon_client,
                     std::string instance_id, std::string gateway_push_addr)
    : _iocPool(ioc_pool_size), _acceptor_ioc(1),
      acceptor_(_acceptor_ioc, tcp::endpoint(tcp::v4(), port)),
      backbon_client_(std::move(backbon_client)),
      instance_id_(std::move(instance_id)),
      gateway_push_addr_(std::move(gateway_push_addr)) {
  // Clients 在 init_and_listen() 中异步初始化（需要 co_await 服务发现）
  spdlog::info("WebServer instance {} created, port {}", instance_id_, port);

  // 初始化TLS
  ssl_ctx_.use_certificate_chain_file("/root/ape-server/scripts/certs/server.crt");
  ssl_ctx_.use_private_key_file("/root/ape-server/scripts/certs/server.key", asio::ssl::context::pem);
}

WebServer::~WebServer() = default;

boost::asio::awaitable<void> WebServer::init_and_listen() {
  // 异步发现所有下游服务
  std::string auth_addr = co_await discover_Service("AuthService", "localhost:50051");
  std::string msg_addr = co_await discover_Service("MsgService", "localhost:50053");
  std::string push_addr = co_await discover_Service("PushService", "localhost:50054");
  std::string group_addr = co_await discover_Service("GroupService", "localhost:50056");

  auth_channel_ = grpc::CreateChannel(auth_addr, grpc::InsecureChannelCredentials());
  auth_client_ = std::make_unique<AuthClient>(auth_channel_);
  msg_channel_ = grpc::CreateChannel(msg_addr, grpc::InsecureChannelCredentials());
  msg_client_ = std::make_unique<MsgClient>(msg_channel_);
  push_channel_ = grpc::CreateChannel(push_addr, grpc::InsecureChannelCredentials());
  push_client_ = std::make_unique<PushClient>(push_channel_);
  group_channel_ = grpc::CreateChannel(group_addr, grpc::InsecureChannelCredentials());
  group_client_ = std::make_unique<GroupClient>(group_channel_);
  auto friend_address = co_await discover_Service("FriendService", "localhost:50057");
  friend_channel_ = grpc::CreateChannel(friend_address, grpc::InsecureChannelCredentials());
  friend_client_ = std::make_unique<FriendClient>(friend_channel_);
  auto conversation_address = co_await discover_Service("ConversationService", "localhost:50058");
  conversation_channel_ = grpc::CreateChannel(conversation_address, grpc::InsecureChannelCredentials());
  conversation_client_ = std::make_unique<ConversationClient>(conversation_channel_);

  // 本实例的推送路由（跨实例互踢 + 三态推送）
  router_ = std::make_unique<GatewayRouter>(
      [this]() { return backbon_client_.get(); }, gateway_push_addr_);

  spdlog::info("WebServer: all downstream services discovered, starting listener on port {} "
               "(instance {})",
               acceptor_.local_endpoint().port(), instance_id_);

  co_await listener();
}

void WebServer::start() {
  // 注册与监听 SIGINT 和 SIGTERM 信号
  boost::asio::signal_set signals(get_acceptor_ioc(), SIGINT, SIGTERM);
  signals.async_wait([this](auto, auto) { stop(); });

  // 启动IOCPool
  _iocPool.startPool();

  // 在 acceptor_ioc 上启动协程：先异步发现服务，再进入监听循环
  boost::asio::co_spawn(get_acceptor_ioc(), init_and_listen(), boost::asio::detached);
  get_acceptor_ioc().run();
}

// 优雅下线（改造方案 4.8）：顺序不能变，注销必须早于关 acceptor
void WebServer::stop() {
  if (stopping_.exchange(true)) {
    return;
  }
  spdlog::info("WebServer: draining instance {}", instance_id_);

  // 1) 从 etcd 注销本实例，新登录不再被路由到本实例
  if (drain_handler_) {
    drain_handler_();
  }

  // 2) 关闭 WSS acceptor，不再接受新连接
  boost::system::error_code ec;
  acceptor_.close(ec);

  // 3) 给所有本地 session 发 SERVER_DRAINING（客户端应立即重连）
  WSSessionManager::instance().notifyDraining();

  // 4) 停止路由续期：draining 标志已置位，心跳不再 CAS
  // 5) 等待 grace，让客户端完成重连
  const int grace = DrainGraceSeconds();
  if (grace > 0) {
    spdlog::info("WebServer: waiting {}s grace before closing sessions", grace);
    std::this_thread::sleep_for(std::chrono::seconds(grace));
  }

  // 6) 强制关闭剩余连接
  WSSessionManager::instance().closeAll();

  // 7) 停止 IO 线程
  _iocPool.stopPool();
  get_acceptor_ioc().stop();
  spdlog::info("WebServer: drained");
}

// 处理单个 WebSocket 连接：接受 TCP 后升级为 WebSocket，然后交给 WSSession 管理
awaitable<void> handle_ws_session(asio::ssl::stream<tcp::socket> stream, AuthClient *auth_client,
                                  MsgClient *msg_client,
                                  PushClient *push_client,
                                  GroupClient *group_client, FriendClient *friend_client,
                                  ConversationClient *conversation_client,
                                  GatewayRouter *router,
                                  std::string instance_id) {
  auto session = std::make_shared<WSSession>(std::move(stream), auth_client,
                                              msg_client, push_client,
                                              group_client, friend_client,
                                              conversation_client, router,
                                              std::move(instance_id));
  co_await session->start();
}

// 使用 acceptor_ioc 进行监听，将接收到的连接投递到 iocPool 进行处理
asio::awaitable<void> WebServer::listener() {
  int nextWorker = 0;
  for (;;) {
    try {
      // 异步接受连接
      auto executor = asio::make_strand(_iocPool.get_ioc(nextWorker).get_executor());
      nextWorker = (nextWorker + 1) % _iocPool.pool_size();
      tcp::socket socket = co_await acceptor_.async_accept(executor, use_awaitable);

      // 用裸 socket 构造一个 ssl::stream
      asio::ssl::stream<tcp::socket> ssl_stream(std::move(socket), ssl_ctx_);

      // 启动协程处理 WebSocket 连接（投递到 iocPool 的某个 worker ioc 上）
      asio::co_spawn(executor,
          handle_ws_session(std::move(ssl_stream), auth_client_.get(),
                            msg_client_.get(), push_client_.get(),
                            group_client_.get(), friend_client_.get(), conversation_client_.get(),
                            router_.get(), instance_id_),
          asio::detached);
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
  co_return;
}
