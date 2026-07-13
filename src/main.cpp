#include "gateway/webserver.h"
#include "messagequeue/kafkaconsumer.h"
#include "messagequeue/kafkaproducer.h"
#include "services/auth_service/server.h"
#include "services/backbon_service/client.h"
#include "services/backbon_service/server.h"
#include "services/gateway_push_service/server.h"
#include "services/msg_service/server.h"
#include "services/group_service/server.h"
#include "services/push_service/server.h"
#include "util/otel_logger.h"
#include "util/otel_metrics.h"
#include "util/otel_tracer.h"
#include "util/redisconnector.h"
#include "util/mysqlconnector.h"
#include "util/ioc_pool.h"
#include "util/mongoconnector.h"
#include "util/snowflake.h"
#include <boost/asio.hpp>
#include <spdlog/spdlog.h>

#include <memory>

int main() {
  // spdlog::set_level(spdlog::level::debug);  // 显示 debug 及以上级别的日志

  try {
    // ── 初始化 OpenTelemetry Tracer + Meter + Logger ──
    ape::otel::InitTracer("ape-server");
    ape::otel::InitMeter("ape-server");
    ape::otel::InitLogger();

    // ── 初始化 Snowflake ID 生成器 ──
    Snowflake::init(std::getenv("SNOWFLAKE_WORKER_ID")
                        ? std::stoll(std::getenv("SNOWFLAKE_WORKER_ID"))
                        : 1,
                    std::getenv("SNOWFLAKE_DATACENTER_ID")
                        ? std::stoll(std::getenv("SNOWFLAKE_DATACENTER_ID"))
                        : 1);

    // ── 初始化 Redis 连接池 ──
    RedisConnector::instance().start(4);

    // ── 初始化 MySQL 连接池 ──
    MySQLConnector::instance().start(2);

    // ── 初始化 MongoDB 连接池 ──
    MongoConnector::instance().start(4);

    // ── 1. 先启动 BackbonService（etcd 服务注册与发现的基础设施）──
    spdlog::info("=== Starting BackbonService on 0.0.0.0:50052 ===");
    auto backbon_server =
        std::make_unique<BackbonServiceImpl>("BackbonService", "0.0.0.0:50052");
    backbon_server->Start();

    // 等待 BackbonService 就绪（确保其他服务注册时 BackbonService 已启动）
    spdlog::info("Waiting for BackbonService to be ready...");
    int counter = 0;
    while (backbon_server->GetState() != ServiceState::kRunning) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds((1 << counter) * 1000));
      ++counter;
      if (counter == 3) {
        spdlog::warn("BackbonService retry failed.");
        // 清理 OTel,导出未发送的 Span + Metrics
        ape::otel::CleanupTracer();
        ape::otel::CleanupMeter();
        return 0;
      }
    }
    spdlog::info("BackbonService is ready");

    // ── 2. 启动 AuthService，通过 BackbonService 注册到 etcd ──
    spdlog::info("=== Starting AuthService on 0.0.0.0:50051 ===");
    auto auth_server =
        std::make_unique<AuthServiceImpl>("AuthService", "0.0.0.0:50051");
    auth_server->EnableEtcdRegistration("localhost:50052",
                                        {"Register", "Login", "ValidateToken"});
    auth_server->Start();

    // ── 3. 启动 MsgService，通过 BackbonService 注册到 etcd ──
    spdlog::info("=== Starting MsgService on 0.0.0.0:50053 ===");
    auto msg_server =
        std::make_unique<MsgServiceImpl>("MsgService", "0.0.0.0:50053");
    msg_server->EnableEtcdRegistration("localhost:50052",
                                       {"GetMaxSeq",
                                        "GetMaxSeqs",
                                        "GetHasReadSeqs",
                                        "GetMsgByConversationIDs",
                                        "GetConversationMaxSeq",
                                        "PullMessageBySeqs",
                                        "GetSeqMessage",
                                        "SearchMessage",
                                        "SendMessages",
                                        "SendSimpleMsg",
                                        "SetUserConversationsMinSeq",
                                        "ClearConversationsMsg",
                                        "UserClearAllMsg",
                                        "DeleteMsgs",
                                        "DeleteMsgPhysicalBySeq",
                                        "DeleteMsgPhysical",
                                        "SetSendMsgStatus",
                                        "GetSendMsgStatus",
                                        "RevokeMsg",
                                        "MarkMsgsAsRead",
                                        "MarkConversationAsRead",
                                        "SetConversationHasReadSeq",
                                        "GetConversationsHasReadAndMaxSeq",
                                        "GetActiveUser",
                                        "GetActiveGroup",
                                        "GetServerTime",
                                        "ClearMsg",
                                        "DestructMsgs",
                                        "GetActiveConversation",
                                        "SetUserConversationMaxSeq",
                                        "GetLastMessageSeqByTime",
                                        "GetLastMessage"});
    msg_server->Start();

    // ── 3.5. 启动 GroupService，通过 BackbonService 注册到 etcd ──
    spdlog::info("=== Starting GroupService on 0.0.0.0:50056 ===");
    auto group_server =
        std::make_unique<GroupServiceImpl>("GroupService", "0.0.0.0:50056");
    group_server->EnableEtcdRegistration("localhost:50052",
                                         {"CreateGroup",
                                          "GetGroupInfo",
                                          "GetGroupMembers",
                                          "JoinGroup",
                                          "LeaveGroup",
                                          "InviteMembers",
                                          "KickMembers",
                                          "DismissGroup",
                                          "UpdateGroupInfo",
                                          "GetUserGroups",
                                          "GetGroupMemberCount"});
    group_server->Start();

    // ── 4. 启动 GatewayPushServer（供 PushService 通过 gRPC 调用推送至
    // WebSocket）──
    spdlog::info("=== Starting GatewayPushServer on 0.0.0.0:50055 ===");
    auto gateway_push_server = std::make_unique<GatewayPushServer>(
        "GatewayPushService", "0.0.0.0:50055");
    gateway_push_server->EnableEtcdRegistration("localhost:50052",
                                                {"PushToUser"});
    gateway_push_server->Start();

    // ── 5. 创建消息队列生产者和消费者（依赖注入）──
    // 创建 Kafka 生产者（由 main 持有，与进程同寿）
    auto producer = std::make_unique<KafkaProducer>("msg_topic");
    msg_server->SetProducer(producer.get());
    spdlog::info("Kafka producer created and injected into MsgService");

    // 创建 Kafka 消费专用的 IOC_Pool（独立于 Redis 的 io_context）
    IOC_Pool kafka_ioc_pool(4);
    kafka_ioc_pool.startPool();
    spdlog::info("Kafka IOC_Pool started with 4 workers");

    // ── 6. 启动 PushService，通过 BackbonService 注册到 etcd ──
    spdlog::info("=== Starting PushService on 0.0.0.0:50054 ===");
    auto push_server =
        std::make_unique<PushServer>("PushService", "0.0.0.0:50054");
    push_server->EnableEtcdRegistration("localhost:50052",
                                        {"PushMsg", "DelUserPushToken"});

    // 创建 Kafka 消费者并注入到 PushServer（通过抽象接口）
    auto consumer = std::make_unique<KafkaConsumer>(
        "push-service-group", std::vector<std::string>{"msg_topic"});
    push_server->SetConsumer(std::move(consumer));
    push_server->SetIOCPool(&kafka_ioc_pool);

    push_server->Start();

    // ── 等待所有需要注册的服务完成 etcd 注册后再启动网关 ──
    spdlog::info("=== Waiting for all services to register to etcd... ===");
    if (!auth_server->WaitForEtcdRegistration(30)) {
      spdlog::warn("AuthService etcd registration timeout or failed, continuing anyway");
    }
    if (!msg_server->WaitForEtcdRegistration(30)) {
      spdlog::warn("MsgService etcd registration timeout or failed, continuing anyway");
    }
    if (!gateway_push_server->WaitForEtcdRegistration(30)) {
      spdlog::warn("GatewayPushService etcd registration timeout or failed, continuing anyway");
    }
    if (!push_server->WaitForEtcdRegistration(30)) {
      spdlog::warn("PushService etcd registration timeout or failed, continuing anyway");
    }
    if (!group_server->WaitForEtcdRegistration(30)) {
      spdlog::warn("GroupService etcd registration timeout or failed, continuing anyway");
    }
    spdlog::info("=== All services registered to etcd, starting gateway ===");

    // ── 启动 WebServer 网关 ──
    spdlog::info("=== Starting WebServer Gateway on port 6666 ===");
    // 创建 BackbonClient 供 WebServer 通过 etcd 服务发现
    auto backbon_channel = grpc::CreateChannel(
        "localhost:50052", grpc::InsecureChannelCredentials());
    auto backbon_client_for_web =
        std::make_unique<BackbonClient>(backbon_channel);
    WebServer server(4, 6666, std::move(backbon_client_for_web));
    server.start();

    // ── 清理所有服务 ──
    spdlog::info(" WebServer Gateway closed...");

    // 清理 OTel,导出未发送的 Span + Metrics
    ape::otel::CleanupTracer();
    ape::otel::CleanupMeter();

  } catch (const std::exception &e) {
    spdlog::error("Main error: {}", e.what());
  }
  return 0;
}