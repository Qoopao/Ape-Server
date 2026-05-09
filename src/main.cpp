#include <boost/asio.hpp>
#include <spdlog/spdlog.h>
#include "gateway/webserver.h"
#include "services/auth_service/server.h"
#include "services/backbon_service/server.h"
#include "services/msg_service/server.h"
#include "services/push_service/server.h"

#include <memory>
#include <thread>

int main()
{
    try
    {
        // ── 启动 AuthService gRPC 服务器 ──
        spdlog::info("=== Starting AuthService on 0.0.0.0:50051 ===");
        auto auth_server = std::make_unique<AuthServiceImpl>(
            "AuthService", "0.0.0.0:50051");
        std::thread auth_thread([&auth_server]() {
            auth_server->Start();
        });
        spdlog::info("AuthService thread created");

        // ── 启动 BackbonService gRPC 服务器 ──
        spdlog::info("=== Starting BackbonService on 0.0.0.0:50052 ===");
        auto backbon_server = std::make_unique<BackbonServiceImpl>(
            "BackbonService", "0.0.0.0:50052");
        std::thread backbon_thread([&backbon_server]() {
            backbon_server->Start();
        });
        spdlog::info("BackbonService thread created");

        // ── 启动 MsgService gRPC 服务器 ──
        spdlog::info("=== Starting MsgService on 0.0.0.0:50053 ===");
        auto msg_server = std::make_unique<MsgServiceImpl>(
            "MsgService", "0.0.0.0:50053");
        std::thread msg_thread([&msg_server]() {
            msg_server->Start();
        });
        spdlog::info("MsgService thread created");

        // ── 启动 PushService gRPC 服务器 ──
        spdlog::info("=== Starting PushService on 0.0.0.0:50054 ===");
        auto push_server = std::make_unique<PushServer>(
            "PushService", "0.0.0.0:50054");
        std::thread push_thread([&push_server]() {
            push_server->Start();
        });
        spdlog::info("PushService thread created");

        // ── 启动 WebServer 网关 (WebSocket / HTTP) ──
        spdlog::info("=== Starting WebServer Gateway on port 6666 ===");
        boost::asio::io_context ioc(1);

        // 监听 SIGINT 和 SIGTERM 信号
        boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
        signals.async_wait([&](auto, auto) { ioc.stop(); });

        WebServer server(ioc, 6666);

        boost::asio::co_spawn(
            ioc,
            server.listener(),
            boost::asio::detached
        );

        ioc.run();

        // ── 清理所有服务 ──
        spdlog::info("WebServer stopped, shutting down all services...");

        spdlog::info("Stopping AuthService...");
        auth_server->Stop();
        if (auth_thread.joinable()) {
            auth_thread.join();
        }
        spdlog::info("AuthService stopped");

        spdlog::info("Stopping BackbonService...");
        backbon_server->Stop();
        if (backbon_thread.joinable()) {
            backbon_thread.join();
        }
        spdlog::info("BackbonService stopped");

        spdlog::info("Stopping MsgService...");
        msg_server->Stop();
        if (msg_thread.joinable()) {
            msg_thread.join();
        }
        spdlog::info("MsgService stopped");

        spdlog::info("Stopping PushService...");
        push_server->Stop();
        if (push_thread.joinable()) {
            push_thread.join();
        }
        spdlog::info("PushService stopped");
    }
    catch (const std::exception &e)
    {
        spdlog::error("Main error: {}", e.what());
    }
    return 0;
}