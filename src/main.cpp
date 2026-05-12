#include <boost/asio.hpp>
#include <spdlog/spdlog.h>
#include "gateway/webserver.h"
#include "services/auth_service/server.h"
#include "services/backbon_service/client.h"
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
        // ── 1. 先启动 BackbonService（etcd 服务注册与发现的基础设施）──
        spdlog::info("=== Starting BackbonService on 0.0.0.0:50052 ===");
        auto backbon_server = std::make_unique<BackbonServiceImpl>(
            "BackbonService", "0.0.0.0:50052");
        backbon_server->Start();

        // 等待 BackbonService 就绪（确保其他服务注册时 BackbonService 已启动）
        spdlog::info("Waiting for BackbonService to be ready...");
        while (backbon_server->GetState() != ServiceState::kRunning) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        spdlog::info("BackbonService is ready");

        // ── 2. 启动 AuthService，通过 BackbonService 注册到 etcd ──
        spdlog::info("=== Starting AuthService on 0.0.0.0:50051 ===");
        auto auth_server = std::make_unique<AuthServiceImpl>(
            "AuthService", "0.0.0.0:50051");
        auth_server->EnableEtcdRegistration("localhost:50052",
            {"Register", "Login", "ValidateToken"});
        auth_server->Start();

        // ── 3. 启动 MsgService，通过 BackbonService 注册到 etcd ──
        spdlog::info("=== Starting MsgService on 0.0.0.0:50053 ===");
        auto msg_server = std::make_unique<MsgServiceImpl>(
            "MsgService", "0.0.0.0:50053");
        msg_server->EnableEtcdRegistration("localhost:50052",
            {"GetMaxSeq", "GetMaxSeqs", "GetHasReadSeqs",
             "GetMsgByConversationIDs", "GetConversationMaxSeq",
             "PullMessageBySeqs", "GetSeqMessage", "SearchMessage",
             "SendMessages", "SendSimpleMsg",
             "SetUserConversationsMinSeq", "ClearConversationsMsg",
             "UserClearAllMsg", "DeleteMsgs", "DeleteMsgPhysicalBySeq",
             "DeleteMsgPhysical", "SetSendMsgStatus", "GetSendMsgStatus",
             "RevokeMsg", "MarkMsgsAsRead", "MarkConversationAsRead",
             "SetConversationHasReadSeq", "GetConversationsHasReadAndMaxSeq",
             "GetActiveUser", "GetActiveGroup", "GetServerTime",
             "ClearMsg", "DestructMsgs", "GetActiveConversation",
             "SetUserConversationMaxSeq", "GetLastMessageSeqByTime",
             "GetLastMessage"});
        msg_server->Start();

        // ── 4. 启动 PushService，通过 BackbonService 注册到 etcd ──
        spdlog::info("=== Starting PushService on 0.0.0.0:50054 ===");
        auto push_server = std::make_unique<PushServer>(
            "PushService", "0.0.0.0:50054");
        push_server->EnableEtcdRegistration("localhost:50052",
            {"PushMsg", "DelUserPushToken"});
        push_server->Start("push-service-group",
                           {"msg_topic"});

        // ── 启动 WebServer 网关 (WebSocket / HTTP) ──
        spdlog::info("=== Starting WebServer Gateway on port 6666 ===");
        boost::asio::io_context ioc(1);

        // 监听 SIGINT 和 SIGTERM 信号
        boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
        signals.async_wait([&](auto, auto) { ioc.stop(); });

        // 创建 BackbonClient 供 WebServer 通过 etcd 服务发现
        auto backbon_channel = grpc::CreateChannel(
            "localhost:50052", grpc::InsecureChannelCredentials());
        auto backbon_client_for_web = std::make_unique<BackbonClient>(backbon_channel);

        WebServer server(ioc, 6666, std::move(backbon_client_for_web));

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