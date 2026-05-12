#include <boost/asio.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/beast.hpp>
#include <spdlog/spdlog.h>
#include <boost/asio/this_coro.hpp> // 确保包含this_coro头文件

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
#include "services/msg_service/client.h"
#include "services/push_service/client.h"
#include "services/backbon_service/client.h"

WebServer::WebServer(asio::io_context &ioc, uint16_t port,
                     std::unique_ptr<BackbonClient> backbon_client)
    : ioc_(ioc), acceptor_(ioc, tcp::endpoint(tcp::v4(), port)),
      backbon_client_(std::move(backbon_client))
{
    // 通过 BackbonService (etcd) 服务发现获取 AuthService 地址
    std::string auth_addr = "localhost:50051"; // fallback
    try {
        auto resp = backbon_client_->GetServicesList("AuthService");
        if (resp.registered() && resp.ipport_size() > 0) {
            auth_addr = resp.ipport(0);
            spdlog::info("WebServer: discovered AuthService at {}", auth_addr);
        } else {
            spdlog::warn("WebServer: AuthService not found in etcd, using fallback {}", auth_addr);
        }
    } catch (const std::exception& e) {
        spdlog::error("WebServer: failed to discover AuthService: {}, using fallback", e.what());
    }
    auth_channel_ = grpc::CreateChannel(auth_addr,
                                        grpc::InsecureChannelCredentials());
    auth_client_ = std::make_unique<AuthClient>(auth_channel_);

    // 通过 BackbonService (etcd) 服务发现获取 MsgService 地址
    std::string msg_addr = "localhost:50053"; // fallback
    try {
        auto resp = backbon_client_->GetServicesList("MsgService");
        if (resp.registered() && resp.ipport_size() > 0) {
            msg_addr = resp.ipport(0);
            spdlog::info("WebServer: discovered MsgService at {}", msg_addr);
        } else {
            spdlog::warn("WebServer: MsgService not found in etcd, using fallback {}", msg_addr);
        }
    } catch (const std::exception& e) {
        spdlog::error("WebServer: failed to discover MsgService: {}, using fallback", e.what());
    }
    msg_channel_ = grpc::CreateChannel(msg_addr,
                                       grpc::InsecureChannelCredentials());
    msg_client_ = std::make_unique<MsgClient>(msg_channel_);

    // 通过 BackbonService (etcd) 服务发现获取 PushService 地址
    std::string push_addr = "localhost:50054"; // fallback
    try {
        auto resp = backbon_client_->GetServicesList("PushService");
        if (resp.registered() && resp.ipport_size() > 0) {
            push_addr = resp.ipport(0);
            spdlog::info("WebServer: discovered PushService at {}", push_addr);
        } else {
            spdlog::warn("WebServer: PushService not found in etcd, using fallback {}", push_addr);
        }
    } catch (const std::exception& e) {
        spdlog::error("WebServer: failed to discover PushService: {}, using fallback", e.what());
    }
    push_channel_ = grpc::CreateChannel(push_addr,
                                        grpc::InsecureChannelCredentials());
    push_client_ = std::make_unique<PushClient>(push_channel_);

    spdlog::info("WebSocketServer initialized, listening on port {}", port);
}

WebServer::~WebServer() = default;

// 停止服务器（优化：处理error_code，避免抛异常）
void WebServer::stop()
{
    acceptor_.close();
}

// 处理单个 WebSocket 连接：接受 TCP 后升级为 WebSocket，然后交给 WSSession 管理
awaitable<void> handle_ws_session(tcp::socket socket, AuthClient* auth_client,
                                   MsgClient* msg_client, PushClient* push_client)
{
    auto session = std::make_shared<WSSession>(std::move(socket), auth_client,
                                                msg_client, push_client);
    co_await session->start();
}

// 监听逻辑（优化：简化executor，增强可读性）
asio::awaitable<void> WebServer::listener()
{

    for (;;)
    {
        try {
            // 异步接受连接
            tcp::socket socket = co_await acceptor_.async_accept(use_awaitable);
            // 启动协程处理 WebSocket 连接（用ioc_作为executor，更直观）
            co_spawn(ioc_, handle_ws_session(std::move(socket), auth_client_.get(),
                                              msg_client_.get(), push_client_.get()), detached);
        }
        catch (const std::exception& e)
        {
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