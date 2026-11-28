#include "gateway/webserver.h"
#include "messagequeue/kafkaconsumer.h"
#include "messagequeue/kafkaproducer.h"
#include "services/auth_service/server.h"
#include "services/backbon_service/client.h"
#include "services/backbon_service/server.h"
#include "services/conversation_service/server.h"
#include "services/friend_service/server.h"
#include "services/gateway_push_service/server.h"
#include "services/group_service/server.h"
#include "services/msg_service/server.h"
#include "services/push_service/server.h"
#include "gateway/ws_session_manager.h"
#include "om/otel_config.h"
#include "om/otel_logger.h"
#include "om/otel_metrics.h"
#include "om/otel_tracer.h"
#include "util/health_server.h"
#include "storage/mongoconnector.h"
#include "storage/mysqlconnector.h"
#include "storage/mysqlhandler.h"
#include "storage/redisconnector.h"
#include "util/instance_id.h"
#include "util/ioc_pool.h"
#include "util/snowflake.h"
#include <boost/asio/use_future.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_shutdown{false};

void OnSignal(int) { g_shutdown.store(true); }

// 启动配置，按role区分网关与各个服务，默认配置为一个进程启动网关+全部服务（all）
struct Config {
  std::string role = "all";
  uint16_t ws_port = 6666;
  uint16_t push_port = 50055;
  uint16_t health_port = 0;
  std::string backbon_endpoint = "localhost:50052";
};

// 获取启动参数的值
std::string ArgValue(int argc, char **argv, const std::string &key) {
  const std::string prefix = "--" + key + "=";
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind(prefix, 0) == 0){
      return arg.substr(prefix.size());
    }
  }
  return {};
}

// 端口号的值
uint16_t PortArg(int argc, char **argv, const std::string &key, uint16_t fallback) {
  const std::string value = ArgValue(argc, argv, key);
  if (value.empty()){
    return fallback;
  }

  try {
    const auto port = std::stoul(value);
    if (port == 0 || port > 65535){
      return fallback;
    }
    return static_cast<uint16_t>(port);
  } catch (const std::exception &) {
    return fallback;
  }
}

// 解析host:port
bool ParseEndpoint(const std::string &endpoint, uint16_t &port) {
  const auto pos = endpoint.rfind(':');
  if (pos == std::string::npos || pos == 0 || pos + 1 >= endpoint.size()){
    return false;
  }
  const std::string port_text = endpoint.substr(pos + 1);
  if (port_text.find_first_not_of("0123456789") != std::string::npos){
    return false;
  }
  try {
    const auto value = std::stoul(port_text);
    if (value == 0 || value > 65535){
      return false;
    }
    port = static_cast<uint16_t>(value);
  } catch (const std::exception &) {
    return false;
  }
  return true;
}

// 取端点端口。调用前必须已通过 ParseEndpoint 校验（由 ParseConfig 保证）。
uint16_t EndpointPort(const std::string &endpoint) {
  uint16_t port = 0;
  ParseEndpoint(endpoint, port);
  return port;
}

// 从启动参数获得配置信息并校验。失败返回 false，main以退出码2终止。
bool ParseConfig(int argc, char **argv, Config &config) {
  const std::string role = ArgValue(argc, argv, "role");
  if (!role.empty()){
    config.role = role;
  }
  config.ws_port = PortArg(argc, argv, "ws-port", config.ws_port);
  config.push_port = PortArg(argc, argv, "push-port", config.push_port);
  if (const char *health = std::getenv("HEALTH_PORT")) {
    try {
      config.health_port = static_cast<uint16_t>(std::stoul(health));
    } catch (const std::exception &) {
    }
  }
  config.health_port = PortArg(argc, argv, "health-port", config.health_port);

  const std::string endpoint = ArgValue(argc, argv, "backbon");
  if (!endpoint.empty()){
    config.backbon_endpoint = endpoint;
  }
  uint16_t validated_port = 0;
  if (!ParseEndpoint(config.backbon_endpoint, validated_port)) {
    spdlog::error("Invalid --backbon={} (expected host:port, e.g. localhost:50052)",
                  config.backbon_endpoint);
    return false;
  }
  return true;
}

std::string Listen(uint16_t port) {
  return "0.0.0.0:" + std::to_string(port);
}

// 群事件 topic（key=groupID）；默认 group_topic，运维可用 GROUP_TOPIC 覆盖。
std::string GroupTopicName() {
  const char *value = std::getenv("GROUP_TOPIC");
  return value != nullptr && *value ? std::string(value) : std::string("group_topic");
}

// 初始化所有role都需要的组件
void InitCore(const std::string &role) {
  // 初始化OpenTelemetry Tracer、 Meter、 Logger（service.name 按 role 区分）
  const auto otel_config = ape::otel::LoadOtelConfig(role);
  ape::otel::InitTracer(otel_config);
  ape::otel::InitMeter(otel_config);
  ape::otel::InitLogger(otel_config);
  spdlog::info("otel config: {}", ape::otel::DescribeOtelConfig(otel_config));

  // 初始化Snowflake ID生成器
  Snowflake::init(std::getenv("SNOWFLAKE_WORKER_ID")
                      ? std::stoll(std::getenv("SNOWFLAKE_WORKER_ID"))
                      : 1,
                  std::getenv("SNOWFLAKE_DATACENTER_ID")
                      ? std::stoll(std::getenv("SNOWFLAKE_DATACENTER_ID"))
                      : 1);

  // Redis连接池
  RedisConnector::instance().start(4);
}

// MySQL是所有业务角色的公共依赖；
// MongoDB只有写/读消息正文的角色（all、gateway、msg、push等）需要。
void InitStorage(bool with_mongo) {
  // MySQL连接池数据库表幂等初始化
  MySQLConnector::instance().start(2);
  boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(0),
                        MySQLHandler::InitializeConversations(),
                        boost::asio::use_future).get();
  boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(0),
                        MySQLHandler::InitializeOutbox(),
                        boost::asio::use_future).get();
  boost::asio::co_spawn(MySQLConnector::instance().get_pool_executor(0),
                        MySQLHandler::InitializeGroups(),
                        boost::asio::use_future).get();

  if (!with_mongo){     // 如果角色不需要Mongo，这里退出就行
    return;
  }
  MongoConnector::instance().start(4);
}

bool RoleNeedsMongo(const std::string &role) {
  // group 需要 Mongo：245 群历史 + outbox worker 写正文。
  return role == "all" || role == "gateway" || role == "msg" ||
         role == "push" || role == "group";
}

void WaitForShutdown() {
  while (!g_shutdown.load()){
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}

// 关键服务注册失败时默认拒绝启动
// APE_ALLOW_UNREGISTERED=1 降级为告警继续。
bool AllowUnregistered() {
  const char *value = std::getenv("APE_ALLOW_UNREGISTERED");
  return value != nullptr && (std::string(value) == "1" || std::string(value) == "true");
}


/*
  启动、等待就绪注册：
  1) 先 DeferRegistration，等 after_start（如 StartOutbox）完成后再 MarkReady，避免 etcd 已可见但服务自身还没初始化完；
  2) required=true的服务（msg/push/gateway push）注册失败时直接失败。

  DeferRegistration()               注册线程先挂起
  Start()                           起gRPC服务线程（异步返回）
  after_start()                     子类自己的初始化（outbox/delivery）
  MarkReady()                       放行注册
  WaitForEtcdRegistration(30)       等注册结果（失败/绑定失败会提前返回 false）
*/
// 注意这个函数返回true不一定代表成功注册，还要结合具体服务看是否允许在注册失败的情况下继续运行服务
template <typename Service>
bool StartService(Service &service, bool required, std::function<void()> after_start = {}) {
  service.DeferRegistration();
  service.Start();

  // 执行服务自己的初始化代码
  if (after_start){
    after_start();
  }
  service.MarkReady();    // 通知etcd注册线程继续

  if (!service.WaitForEtcdRegistration(30)) {
    if (required && !AllowUnregistered()) {
      spdlog::error("[{}] etcd 注册失败，关键服务拒绝启动", service.GetServiceName());
      return false;
    }
    spdlog::warn("[{}] etcd registration timeout or failed, continuing anyway", service.GetServiceName());
  }
  return true;
}

// 各服务在 etcd 里声明的方法列表：RunXxx 与 RunAll 共用，避免两份清单漂移。
const std::vector<std::string> kAuthMethods = {"Register", "Login",
                                               "ValidateToken"};
const std::vector<std::string> kMsgMethods = {
    "GetSingleChatHistory", "SendMessages"};
const std::vector<std::string> kGroupMethods = {"Execute", "SendGroupMessages",
                                                    "GetGroupChatHistory"};
const std::vector<std::string> kFriendMethods = {"Execute"};
const std::vector<std::string> kConversationMethods = {
    "GetAllConversations", "MarkConversationRead", "UpdateConversationSettings",
    "DeleteConversation"};
const std::vector<std::string> kPushMethods = {"PushMsg", "AckConversations"};
const std::vector<std::string> kGatewayPushMethods = {
    "PushFrame", "PushBatch", "IsOnline", "KickUser"};

// RunAll 的多个服务需要同时等待注册：共享一个截止时间（不是每个服务各等 30s）。
struct RegistrationProbe {
  const char *name;
  bool critical;
  std::function<bool()> registered;
  std::function<bool()> failed_start;
};

template <typename Service>
RegistrationProbe Probe(const char *name, Service &service, bool critical) {
  return {name, critical,
          [&service]() { return service.IsEtcdRegistered(); },
          [&service]() { return service.GetState() == ServiceState::kStopped; }};
}

// 在同一个截止时间内等待所有服务注册。
// 返回false表示有关键服务未注册；非关键服务失败只告警。
bool WaitForRegistrations(const std::vector<RegistrationProbe> &probes, int timeout_seconds) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
  std::vector<bool> settled(probes.size(), false);
  bool critical_ok = true;

  // 轮询
  for (;;) {
    bool pending = false;   // 指示是否仍有未注册的服务
    for (size_t i = 0; i < probes.size(); ++i) {
      if (settled[i]){
        continue;
      }

      const auto &probe = probes[i];
      if (probe.registered()) {
        settled[i] = true;
        spdlog::info("[{}] etcd registration confirmed", probe.name);
      } else if (probe.failed_start()) {
        settled[i] = true;
        if (probe.critical) {
          critical_ok = false;
          spdlog::error("[{}] 服务启动失败（监听端口被占用？），不会注册", probe.name);
        } else {
          spdlog::error("[{}] 服务启动失败，跳过注册等待", probe.name);
        }
      } else {
        pending = true;
      }
    }
    if (!pending){
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline){
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  }

  // 通知调用者
  for (size_t i = 0; i < probes.size(); ++i) {
    if (settled[i]){
      continue;
    }
    if (probes[i].critical) {
      critical_ok = false;
      spdlog::error("[{}] {}s 内未完成 etcd 注册（关键服务）", probes[i].name, timeout_seconds);
    } else {
      spdlog::warn("[{}] {}s 内未完成 etcd 注册，继续启动", probes[i].name, timeout_seconds);
    }
  }

  return critical_ok;
}

// 启动后端骨干服务
int RunBackbon(const Config &config) {
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  const uint16_t backbon_port = EndpointPort(config.backbon_endpoint);
  auto backbon_server = std::make_unique<BackbonServiceImpl>("BackbonService", Listen(backbon_port));
  backbon_server->Start();

  // BackbonService本身不注册自己
  int counter = 0;
  while (backbon_server->GetState() != ServiceState::kRunning) {
    std::this_thread::sleep_for(std::chrono::milliseconds((1 << counter) * 100));
    ++counter;
    if (counter == 3) {
      spdlog::warn("BackbonService start failed.");
      return 1;
    }
  }
  spdlog::info("BackbonService listening on {}", Listen(backbon_port));
  WaitForShutdown();
  backbon_server->Stop();
  return 0;
}

int RunAuth(const Config &config) {
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  auto auth_server = std::make_unique<AuthServiceImpl>("AuthService", Listen(50051));
  auth_server->EnableEtcdRegistration(config.backbon_endpoint, kAuthMethods);
  if (!StartService(*auth_server, false)){
    return 1;
  }
  WaitForShutdown();
  auth_server->Stop();
  return 0;
}

int RunFriend(const Config &config) {
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  auto friend_server = std::make_unique<FriendServiceImpl>("FriendService", Listen(50057));
  friend_server->EnableEtcdRegistration(config.backbon_endpoint, kFriendMethods);
  if (!StartService(*friend_server, false)){
    return 1;
  }
  WaitForShutdown();
  friend_server->Stop();
  return 0;
}

int RunConversation(const Config &config) {
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  auto conversation_server = std::make_unique<ConversationServiceImpl>("ConversationService", Listen(50058));
  conversation_server->EnableEtcdRegistration(config.backbon_endpoint,
                                              kConversationMethods);

  if (!StartService(*conversation_server, false)){
    return 1;
  }
  WaitForShutdown();
  conversation_server->Stop();
  return 0;
}

int RunMsg(const Config &config) {
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  auto producer = std::make_unique<KafkaProducer>("msg_topic");
  auto msg_server = std::make_unique<MsgServiceImpl>("MsgService", Listen(50053));
  msg_server->EnableEtcdRegistration(config.backbon_endpoint, kMsgMethods);
  msg_server->SetProducer(producer.get());
  if (!StartService(*msg_server, true, [&msg_server]() { msg_server->StartOutbox(); })){
    return 1;
  }
  WaitForShutdown();
  msg_server->StopOutbox();
  msg_server->Stop();
  return 0;
}

int RunPush(const Config &config) {
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  IOC_Pool kafka_ioc_pool(4);
  kafka_ioc_pool.startPool();
  auto push_server = std::make_unique<PushServer>("PushService", Listen(50054));
  push_server->EnableEtcdRegistration(config.backbon_endpoint, kPushMethods);
  auto consumer = std::make_unique<KafkaConsumer>(
      "push-service-group", std::vector<std::string>{"msg_topic"});
  push_server->SetConsumer(std::move(consumer));
  // 群扇出独立消费组：group_topic（key=groupID，单群串行）。
  push_server->SetGroupConsumer(std::make_unique<KafkaConsumer>(
      "push-group-fanout", std::vector<std::string>{GroupTopicName()}));
  push_server->SetIOCPool(&kafka_ioc_pool);
  if (!StartService(*push_server, true)){
    return 1;
  }
  WaitForShutdown();
  push_server->Stop();
  // 先析构 PushServer（join 两个 consumer 线程），再停 Kafka IOC 池。
  push_server.reset();
  kafka_ioc_pool.stopPool();
  return 0;
}

int RunGroup(const Config &config) {
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);
  auto producer = std::make_unique<KafkaProducer>(GroupTopicName());
  auto group_server = std::make_unique<GroupServiceImpl>("GroupService", Listen(50056));
  group_server->EnableEtcdRegistration(config.backbon_endpoint, kGroupMethods);
  group_server->SetProducer(producer.get());
  // outbox worker 起来之后再注册，避免 etcd 已可见但 seq 落库无人推进。
  if (!StartService(*group_server, false, [&group_server]() { group_server->StartOutbox(); })){
    return 1;
  }
  WaitForShutdown();
  group_server->StopOutbox();
  group_server->Stop();
  return 0;
}

// 网关实例（必须同进程启动网关与网关推送服务）：WSS WebServer + GatewayPushService
int RunGateway(const Config &config) {
  const std::string push_listen = Listen(config.push_port);
  const std::string instance_id = ape::instance::ForListenAddress(push_listen);
  ape::instance::SetSelf(instance_id);

  auto gateway_push_server = std::make_unique<GatewayPushServer>("GatewayPushService", push_listen);
  gateway_push_server->EnableEtcdRegistration(
      config.backbon_endpoint, kGatewayPushMethods, instance_id);

  // 网关启动失败直接退出
  if (!StartService(*gateway_push_server, true)){
    return 1;
  }

  auto backbon_channel = grpc::CreateChannel(config.backbon_endpoint,grpc::InsecureChannelCredentials());
  auto backbon_client = std::make_unique<BackbonClient>(backbon_channel);

  WebServer server(4, config.ws_port, std::move(backbon_client),
                   instance_id,
                   ape::instance::AdvertiseAddress(push_listen));

  // 注册优雅下线的处理函数
  server.SetDrainHandler([&gateway_push_server]() {
    if (gateway_push_server){
      gateway_push_server->Stop();
    }
  });

  spdlog::info("=== Gateway {} starting: WS={} GatewayPush={} ===", instance_id, config.ws_port, config.push_port);

  ape::ops::HealthServer health;
  if (config.health_port != 0) {
    if (health.Start(config.health_port,
                     []() { return !WSSessionManager::instance().draining(); }))
      spdlog::info("health server listening on 0.0.0.0:{}", config.health_port);
    else
      spdlog::warn("health server failed to start on port {}", config.health_port);
  }

  server.start();
  health.Stop();
  spdlog::info("Gateway {} stopped", instance_id);
  return 0;
}

// 单进程模式（--role=all）：
// 阶段0 注册中心
// 阶段1 业务服务（各自线程内并行注册
// 阶段2 共享截止等待注册
// 阶段3 网关（阻塞）
// 阶段4 逆序停止。
int RunAll(const Config &config) {
  const uint16_t backbon_port = EndpointPort(config.backbon_endpoint);

  // 阶段0：注册中心
  auto backbon_server = std::make_unique<BackbonServiceImpl>(
      "BackbonService", Listen(backbon_port));
  backbon_server->Start();
  for (int attempt = 0; backbon_server->GetState() != ServiceState::kRunning;
       ++attempt) {
    if (attempt == 3) {
      spdlog::error("BackbonService 启动失败");
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100 << attempt));
  }
  spdlog::info("BackbonService ready on {}", Listen(backbon_port));

  // 阶段1：业务服务（Start 立即返回，注册在各自线程里并行进行）
  auto auth_server = std::make_unique<AuthServiceImpl>("AuthService", Listen(50051));
  auth_server->EnableEtcdRegistration(config.backbon_endpoint, kAuthMethods);
  auth_server->Start();

  auto producer = std::make_unique<KafkaProducer>("msg_topic");
  auto msg_server = std::make_unique<MsgServiceImpl>("MsgService", Listen(50053));
  msg_server->EnableEtcdRegistration(config.backbon_endpoint, kMsgMethods);
  msg_server->SetProducer(producer.get());
  msg_server->DeferRegistration(); // outbox 起来之后再注册
  msg_server->Start();

  auto group_producer = std::make_unique<KafkaProducer>(GroupTopicName());
  auto group_server = std::make_unique<GroupServiceImpl>("GroupService", Listen(50056));
  group_server->EnableEtcdRegistration(config.backbon_endpoint, kGroupMethods);
  group_server->SetProducer(group_producer.get());
  group_server->DeferRegistration(); // outbox 起来之后再注册
  group_server->Start();

  auto friend_server = std::make_unique<FriendServiceImpl>("FriendService", Listen(50057));
  friend_server->EnableEtcdRegistration(config.backbon_endpoint, kFriendMethods);
  friend_server->Start();

  auto conversation_server = std::make_unique<ConversationServiceImpl>("ConversationService", Listen(50058));
  conversation_server->EnableEtcdRegistration(config.backbon_endpoint, kConversationMethods);
  conversation_server->Start();

  auto gateway_push_server = std::make_unique<GatewayPushServer>("GatewayPushService", Listen(config.push_port));
  gateway_push_server->EnableEtcdRegistration(config.backbon_endpoint, kGatewayPushMethods);
  gateway_push_server->Start();
  const std::string instance_id = gateway_push_server->GetInstanceId();
  ape::instance::SetSelf(instance_id);

  // PushService依赖Kafka消费者和独立IO池；IO池必须先于PushServer构造
  IOC_Pool kafka_ioc_pool(4);
  kafka_ioc_pool.startPool();
  auto push_server = std::make_unique<PushServer>("PushService", Listen(50054));
  push_server->EnableEtcdRegistration(config.backbon_endpoint, kPushMethods);
  push_server->SetConsumer(std::make_unique<KafkaConsumer>("push-service-group", std::vector<std::string>{"msg_topic"}));
  push_server->SetGroupConsumer(std::make_unique<KafkaConsumer>("push-group-fanout", std::vector<std::string>{GroupTopicName()}));
  push_server->SetIOCPool(&kafka_ioc_pool);
  push_server->Start();

  // outbox依赖 producer 与 PushService，最后启动
  msg_server->StartOutbox();
  msg_server->MarkReady();
  group_server->StartOutbox();
  group_server->MarkReady();

  // 阶段2：统一等待注册
  spdlog::info("=== Waiting for services to register to etcd (shared {}s) ===", 30);
  const bool critical_ok = WaitForRegistrations(
      {Probe("AuthService", *auth_server, false),
       Probe("MsgService", *msg_server, true),
       Probe("GatewayPushService", *gateway_push_server, true),
       Probe("PushService", *push_server, true),
       Probe("GroupService", *group_server, false),
       Probe("FriendService", *friend_server, false),
       Probe("ConversationService", *conversation_server, false)},
      30);
  if (!critical_ok && !AllowUnregistered()) {
    spdlog::error("关键服务未注册到etcd, 拒绝启动(可用环境变量APE_ALLOW_UNREGISTERED=1强制继续)");
    return 1;
  }

  // 阶段3：网关（阻塞运行，直到SIGINT/SIGTERM信号触发优雅下线）
  auto backbon_client = std::make_unique<BackbonClient>(grpc::CreateChannel(
      config.backbon_endpoint, grpc::InsecureChannelCredentials()));
  WebServer server(
      4, config.ws_port, std::move(backbon_client), instance_id,
      ape::instance::AdvertiseAddress(gateway_push_server->GetListenAddress()));
  server.SetDrainHandler([&gateway_push_server]() {
    if (gateway_push_server)
      gateway_push_server->Stop();
  });
  ape::ops::HealthServer health;
  if (config.health_port != 0) {
    if (health.Start(config.health_port,
                     []() { return !WSSessionManager::instance().draining(); }))
      spdlog::info("health server listening on 0.0.0.0:{}", config.health_port);
    else
      spdlog::warn("health server failed to start on port {}", config.health_port);
  }

  server.start();
  health.Stop();

  // 阶段4：逆序停止（其余服务由析构函数注销并关闭）
  msg_server->StopOutbox();
  group_server->StopOutbox();
  spdlog::info("WebServer Gateway closed...");
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  Config config;
  if (!ParseConfig(argc, argv, config)){
    return 2;
  }

  try {
    InitCore(config.role);
    spdlog::info("ape-server starting with role={}", config.role);

    // backbon只依赖Redis/etcd；其余业务角色按需初始化MySQL/Mongo
    if (config.role != "backbon"){
      InitStorage(RoleNeedsMongo(config.role));
    }
      
    int result = 0;
    if (config.role == "all") {
      result = RunAll(config);
    } else if (config.role == "backbon") {
      result = RunBackbon(config);
    } else if (config.role == "gateway") {
      result = RunGateway(config);
    } else if (config.role == "auth") {
      result = RunAuth(config);
    } else if (config.role == "msg") {
      result = RunMsg(config);
    } else if (config.role == "push") {
      result = RunPush(config);
    } else if (config.role == "group") {
      result = RunGroup(config);
    } else if (config.role == "friend") {
      result = RunFriend(config);
    } else if (config.role == "conversation") {
      result = RunConversation(config);
    } else {
      spdlog::error("Unknown --role={} (expected all|backbon|gateway|auth|msg|"
                    "push|group|friend|conversation)", config.role);
      result = 2;
    }

    ape::otel::CleanupTracer();
    ape::otel::CleanupMeter();
    ape::otel::CleanupLogger();
    return result;
  } catch (const std::exception &e) {
    spdlog::error("Main error: {}", e.what());
    ape::otel::CleanupTracer();
    ape::otel::CleanupMeter();
    ape::otel::CleanupLogger();
    return 1;
  }
}
