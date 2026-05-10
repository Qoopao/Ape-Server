#include <boost/asio.hpp>
#include <spdlog/spdlog.h>
#include "gateway/webserver.h"
#include "services/auth_service/server.h"
#include "services/backbon_service/server.h"
#include "services/msg_service/server.h"
#include "services/push_service/server.h"
#include "util/redishandler.h"

#include <memory>

int main()
{
    // spdlog::set_level(spdlog::level::debug);  // 显示 debug 及以上级别的日志

    try
    {
        // ── 启动 AuthService gRPC 服务器 ──
        spdlog::info("=== Starting AuthService on 0.0.0.0:50051 ===");
        auto auth_server = std::make_unique<AuthServiceImpl>(
            "AuthService", "0.0.0.0:50051");
        auth_server->Start();

        // ── 启动 BackbonService gRPC 服务器 ──
        spdlog::info("=== Starting BackbonService on 0.0.0.0:50052 ===");
        auto backbon_server = std::make_unique<BackbonServiceImpl>(
            "BackbonService", "0.0.0.0:50052");
        backbon_server->Start();

        // ── 启动 MsgService gRPC 服务器 ──
        spdlog::info("=== Starting MsgService on 0.0.0.0:50053 ===");
        auto msg_server = std::make_unique<MsgServiceImpl>(
            "MsgService", "0.0.0.0:50053");
        msg_server->Start();

        // ── 启动 PushService gRPC 服务器 ──
        spdlog::info("=== Starting PushService on 0.0.0.0:50054 ===");
        auto push_server = std::make_unique<PushServer>(
            "PushService", "0.0.0.0:50054");
        push_server->Start("push-service-group",
                           {"msg_topic"});

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
        spdlog::info("关闭Web网关...");

    }
    catch (const std::exception &e)
    {
        spdlog::error("Main error: {}", e.what());
    }
    return 0;
}