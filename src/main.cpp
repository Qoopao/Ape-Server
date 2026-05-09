#include <boost/asio.hpp>
#include <spdlog/spdlog.h>
#include "gateway/webserver.h"
#include "services/auth_service/server.h"

#include <thread>

int main()
{
    try
    {
        // ── 启动 AuthService gRPC 服务器 (独立线程) ──
        spdlog::info("=== Starting AuthService on 0.0.0.0:50051 ===");
        auto auth_server = std::make_unique<AuthServiceImpl>(
            "AuthService", "0.0.0.0:50051");
        std::thread auth_thread([&auth_server]() {
            auth_server->Start();
        });
        spdlog::info("AuthService thread created");

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

        // ── 清理 ──
        spdlog::info("WebServer stopped, shutting down AuthService...");
        auth_server->Stop();
        if (auth_thread.joinable()) {
            auth_thread.join();
        }
        spdlog::info("AuthService stopped");
    }
    catch (const std::exception &e)
    {
        spdlog::error("Main error: {}", e.what());
    }
    return 0;
}
