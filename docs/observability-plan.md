# Ape-Server IM 服务端可观测性方案

> 基于 OpenTelemetry + Jaeger + VictoriaMetrics + Elasticsearch + Grafana 的全栈可观测性方案

## 一、现状分析

### 1. 项目架构总览

```
┌─────────────────────────────────────────────────────────────────┐
│                      Ape-Server (单体多服务)                     │
│                                                                 │
│  WebServer (6666)  ←→  AuthService (50051)                      │
│   WebSocket/HTTP        ├─ Register / Login / ValidateToken      │
│                         ├─ MySQL (用户数据)                      │
│                         │                                       │
│                    ←→  MsgService (50053)                        │
│                         ├─ SendMessages / PullMessageBySeqs ...  │
│                         ├─ MongoDB (消息存储)                    │
│                         ├─ Redis (缓存/Seq)                       │
│                         │                                       │
│                    ←→  PushService (50054)                       │
│                         ├─ Kafka (消息队列)                      │
│                         └─→ GatewayPushService (50055)          │
│                              └─→ WebSocket 推送                 │
│                                                                 │
│  BackbonService (50052) ←→ etcd (服务注册/发现)                 │
└─────────────────────────────────────────────────────────────────┘
```

### 2. 服务端口速查

| 服务 | 端口 | 协议 | 功能 |
|------|------|------|------|
| WebServer | 6666 | WebSocket / HTTP | 客户端长连接网关 |
| AuthService | 50051 | gRPC | 登录 / 注册 / Token 校验 |
| BackbonService | 50052 | gRPC | etcd 服务注册与发现 |
| MsgService | 50053 | gRPC | 消息收发 / 查询 / 已读 |
| PushService | 50054 | gRPC | 消息推送（消费 Kafka） |
| GatewayPushService | 50055 | gRPC | 通过 WebSocket 实时推送 |

### 3. 基础设施依赖

| 组件 | 用途 |
|------|------|
| MySQL | 用户账号数据（AuthService） |
| MongoDB | 消息持久化存储（MsgService） |
| Redis | 缓存、Seq 计数器、会话 |
| Kafka | 消息异步中转（msg_topic, offline_msg_topic） |
| etcd | gRPC 服务注册与发现（BackbonService） |

### 4. 现有可观测性基础设施（已部署）

| 组件 | 端口 | 用途 |
|------|------|------|
| **OTEL Collector** | 4317 | OTLP gRPC 数据接收入口 |
| **Jaeger** | 16686 | 链路追踪可视化 |
| **VictoriaMetrics** | 8428 | 指标时序存储 |
| **Elasticsearch** | 9200 | 日志存储与全文检索 |
| **Grafana** | 3000 | 统一可视化面板 |
| **Kibana** | 5601 | 日志查询与分析 |

### 5. Collector 数据管道（`docker/otel/otel-collector-config.yaml`）

```
Traces:  OTLP(4317) → batch → Jaeger
Metrics: OTLP(4317) → batch → PrometheusRemoteWrite → VictoriaMetrics
Logs:    OTLP(4317) → batch → Elasticsearch (+ console debug)
```

### 6. 当前缺失

项目的 C++ 代码目前**仅使用 `spdlog` 做基础日志输出**，没有集成 OpenTelemetry C++ SDK，因此存在以下问题：

- **没有链路追踪（Trace）**：无法追踪一个客户端请求经过 WebServer → gRPC → Kafka → PushService 的完整路径
- **没有指标暴露（Metrics）**：无法得知各 RPC 的 QPS、延迟分布、错误率，也无法监控 WebSocket 连接数、Kafka 消费 Lag 等
- **日志没有与 Trace 关联**：spdlog 输出的日志缺少 `trace_id` / `span_id`，无法从一条日志快速定位到本次请求的完整调用链

---

## 二、实施方案

### 阶段一：C++ OpenTelemetry SDK 集成

#### 1.1 依赖引入（vcpkg）

在 `vcpkg.json` 中添加依赖：

```json
{
  "dependencies": [
    "opentelemetry-cpp"
  ]
}
```

安装命令：

```bash
./vcpkg install opentelemetry-cpp[otlp-grpc,metrics]:x64-linux
```

#### 1.2 创建 OTel 初始化模块

新建 `include/util/otel_tracer.h` 和 `src/util/otel_tracer.cpp`：

```
功能规划：
├─ InitTracer()        → 初始化 TracerProvider + OTLP gRPC Exporter
├─ InitLogger()        → 初始化 LoggerProvider + OTLP gRPC Exporter
├─ InitMeter()         → 初始化 MeterProvider + OTLP gRPC Exporter
├─ GetTracer()         → 获取 tracer 实例（按服务名）
├─ GetMeter()          → 获取 meter 实例（按服务名）
├─ GetLogger()         → 获取 logger 实例（按服务名）
└─ CleanupOTel()       → 优雅关闭，导出未发送的数据
```

配置方式（通过环境变量），写入 `docker-compose.yml` 或 `.env` 文件：

```bash
OTEL_EXPORTER_OTLP_ENDPOINT=http://localhost:4317
OTEL_SERVICE_NAME=ape-server
OTEL_RESOURCE_ATTRIBUTES=service.namespace=ape-im,service.version=1.0.0
```

### 阶段二：链路追踪埋点

#### 2.1 gRPC Server 端拦截器

修改 `include/services/base_service.h` 的 `RunServer()` 方法，注册 Server 拦截器。

每个 gRPC 请求自动创建 Span：

```
Span Name: "/AuthService/Login"
Attributes:
  ├─ rpc.system = "grpc"
  ├─ rpc.service = "AuthService"
  ├─ rpc.method = "Login"
  ├─ rpc.grpc.status_code = 0
  └─ net.peer.ip = "10.0.0.5"
Status: OK / Error + details
```

拦截器伪代码：

```cpp
class OtelServerInterceptor : public grpc::ServerInterceptor {
  Status Intercept(ServerRpcInfo* info) override {
    auto ctx = info->client_context();
    // 从 gRPC metadata 提取 traceparent
    auto parent_ctx = opentelemetry::context::propagation::Extract(ctx);
    auto span = tracer->StartSpan(info->method(), parent_ctx);
    span->SetAttribute("rpc.system", "grpc");
    // ...
    auto status = info->Proceed();
    span->SetStatus(status);
    span->End();
    return status;
  }
};
```

#### 2.2 gRPC Client 端埋点

在所有 `*Client` 类（AuthClient、MsgClient、PushClient、GatewayPushClient、BackbonClient）中，封装 gRPC 调用时自动创建 Client Span 并注入 W3C TraceContext 到 metadata 中。

```
每个 Client 调用自动创建 Span:
├─ Span Name: "AuthService/Login"
├─ Kind: CLIENT
├─ Attributes:
│   ├─ rpc.service = "AuthService"
│   ├─ rpc.method = "Login"
│   └─ rpc.grpc.status_code
└─ 自动注入 traceparent 到 outgoing metadata
```

#### 2.3 WebSocket/HTTP 网关埋点

在 `WebServer` (include/gateway/webserver.h) 和 `WSSession` (include/gateway/ws_session.h) 中：

1. 收到 WebSocket 消息时，从消息体或 HTTP Header 提取 traceparent
2. 如果不存在 traceparent，创建新的 Root Span
3. 将 Span Context 传递到下游 gRPC 调用

```
每个 WebSocket 消息处理创建 Span:
├─ Span Name: "WS /msg/send" 或 "WS /msg/pull"
├─ Kind: SERVER
├─ Attributes:
│   ├─ ws.message_type = "send_msg" / "pull_msg" / "auth"
│   ├─ ws.user_id = "123456"
│   ├─ ws.operation = "SendMessages"
│   └─ ws.message_size = 1024
└─ 将 traceparent 注入到后续 gRPC 调用的 metadata 中
```

#### 2.4 Kafka 生产者/消费者埋点

在 `KafkaProducer` (include/messagequeue/kafkaproducer.h) 和 `KafkaConsumer` (include/messagequeue/kafkaconsumer.h) 中：

**Producer 端：**

```
Span Name: "Kafka Publish msg_topic"
Attributes:
  ├─ messaging.system = "kafka"
  ├─ messaging.destination = "msg_topic"
  ├─ messaging.message_id = "uuid"
  └─ messaging.message_size = 2048

将 traceparent 写入 Kafka 消息 Header 中以传播上下文
```

**Consumer 端：**

```
Span Name: "Kafka Consume msg_topic"
Attributes:
  ├─ messaging.system = "kafka"
  ├─ messaging.destination = "msg_topic"
  ├─ messaging.kafka.partition = 0
  ├─ messaging.kafka.offset = 12345
  └─ messaging.kafka.consumer_group = "push-service-group"

从 Kafka 消息 Header 中提取 traceparent，
作为 Parent Span 链接到 Producer 的 Span
```

这样可以在 Jaeger 中看到一个完整的链路：
`WebSocket → MsgService → Kafka Producer → Kafka Broker → Kafka Consumer → PushService → GatewayPushService → WebSocket`

#### 2.5 数据库操作埋点

在各 Util 类中添加 Span：

| 模块 | 文件 | Span Name 示例 | 关键属性 |
|------|------|---------------|---------|
| MySQL | `src/util/mysqlconnector.cpp` | `MySQL SELECT users` | db.system, db.operation, db.statement |
| MongoDB | `src/util/mongoconnector.cpp` | `MongoDB find conversations` | db.system, db.collection, db.operation |
| Redis | `src/util/redisconnector.cpp` | `Redis GET user:session` | db.system, db.operation, db.key |
| etcd | `src/util/etcdconnector.cpp` | `etcd RegisterService` | etcd.operation, etcd.key |

### 阶段三：业务指标暴露

#### 3.1 指标定义

创建 `include/util/otel_metrics.h`，定义以下指标：

| 指标名称 | 类型 | 单位 | 标签 | 描述 |
|---------|------|------|------|------|
| `rpc_server_duration_ms` | Histogram | ms | rpc_service, rpc_method, status | gRPC 请求耗时分布（buckets: 1,5,10,25,50,100,250,500,1000） |
| `rpc_server_requests_total` | Counter | - | rpc_service, rpc_method, status_code | gRPC 请求总数 |
| `ws_connections_active` | Gauge | - | - | 当前活跃 WebSocket 连接数 |
| `ws_connections_total` | Counter | - | - | 历史总连接数 |
| `ws_messages_total` | Counter | - | type | WebSocket 消息总数（按 send/pull/auth 分类） |
| `kafka_messages_produced_total` | Counter | - | topic | Kafka 生产消息总数 |
| `kafka_messages_consumed_total` | Counter | - | topic | Kafka 消费消息总数 |
| `kafka_consumer_lag` | Gauge | - | topic, partition, group | Kafka 消费延迟 |
| `db_query_duration_ms` | Histogram | ms | db_system, operation | 数据库查询耗时 |
| `redis_command_duration_ms` | Histogram | ms | command | Redis 命令耗时 |
| `auth_login_total` | Counter | - | status | 登录请求总数（按 success/failure） |
| `msg_send_total` | Counter | - | - | 消息发送总数 |
| `msg_push_latency_ms` | Histogram | ms | - | 消息推送端到端延迟（消息产生 → 推送到客户端） |

#### 3.2 指标集成点

```
BaseServiceServer::RunServer()   → rpc_server_duration_ms, rpc_server_requests_total
WSSessionManager                 → ws_connections_active, ws_connections_total, ws_messages_total
KafkaProducer::send()            → kafka_messages_produced_total
KafkaConsumer::consume()         → kafka_messages_consumed_total, kafka_consumer_lag
MySQLConnector / MongoConnector  → db_query_duration_ms
RedisConnector                   → redis_command_duration_ms
AuthServiceImpl::Login()         → auth_login_total
MsgServiceImpl::SendMessages()   → msg_send_total
PushServer::PushMsg()            → msg_push_latency_ms
```

### 阶段四：日志与 Trace 关联

#### 4.1 spdlog + OTel 桥接

创建一个自定义 `spdlog` sink（`include/util/spdlog_otel_sink.h`），将日志同时输出到：
1. 控制台（原有行为，保留）
2. OTLP Log Exporter → Elasticsearch（自动附加 TraceId/SpanId/ServiceName）

工作方式：
```
spdlog::info("[MsgService] 服务器启动成功")
         ↓
┌────────────────────────────────────────┐
│  spdlog_otel_sink::log(msg)            │
│  1. 从当前 context 获取 trace_id/span_id │
│  2. 构造 OTel LogRecord                │
│  3. 通过 OTLP Exporter 发送到 Collector │
│  4. Collector → Elasticsearch           │
└────────────────────────────────────────┘
```

导出到 Elasticsearch 的日志格式（JSON）：

```json
{
  "timestamp": "2026-05-15T09:30:00.000Z",
  "severity": "INFO",
  "body": "[MsgService] 服务器启动成功",
  "trace_id": "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4",
  "span_id": "1234567890abcdef",
  "service.name": "ape-server",
  "service.namespace": "ape-im",
  "thread.name": "MsgService-thread",
  "log.logger": "default"
}
```

优势：在 Kibana 中可以通过 `trace_id` 搜索到一次完整请求的所有关联日志，再从 Jaeger 中查看该 trace 的调用链拓扑。

### 阶段五：Grafana 可视化面板

#### 5.1 数据源配置

在 Grafana 中添加数据源（通过 provisioning 文件自动配置）：

| 数据源 | 类型 | URL |
|--------|------|-----|
| VictoriaMetrics | Prometheus | `http://victoriametrics:8428` |
| Elasticsearch | Elasticsearch | `http://elasticsearch:9200` |
| Jaeger | Jaeger | `http://jaeger-all-in-one:16686` |

配置文件路径：`docker/otel/grafana/provisioning/datasources/`

#### 5.2 推荐 Dashboard

**Dashboard 1: 服务总览 (IM Service Overview)**

```
┌───────────────────┐ ┌───────────────────┐ ┌───────────────────┐
│ 活跃 WS 连接数      │ │ 总 RPC QPS        │ │ 错误率 (%)         │
│ (Stat Gauge)      │ │ (Stat)            │ │ (Stat)            │
├───────────────────┤ ├───────────────────┤ ├───────────────────┤
│ 消息发送量/分钟     │ │ Kafka 生产/消费速率 │ │ 消息推送 P99 延迟  │
│ (Time Series)     │ │ (Time Series)     │ │ (Stat)            │
├───────────────────┴─┴───────────────────┴─┴───────────────────┤
│                   各服务健康状态 (Table)                        │
│    AuthService / MsgService / PushService / GatewayPush /      │
│    BackbonService / WebServer                                  │
├───────────────────────────────────────────────────────────────┤
│              RPC QPS 趋势 (按服务分组的 Time Series)            │
└───────────────────────────────────────────────────────────────┘
```

**Dashboard 2: gRPC 服务详情 (gRPC Service Details)**

```
├─ RPC Method 请求量堆叠图 (按 status_code)
├─ P50 / P95 / P99 延迟趋势 (按 service 分组)
├─ 错误率趋势 (5分钟窗口)
├─ Top 10 最慢 gRPC 方法 (Table: method, avg_latency, p99_latency, error_rate)
└─ 请求量 / 延迟 热力图
```

**Dashboard 3: 基础设施监控 (Infrastructure Health)**

```
├─ MySQL 操作延迟 (P50/P95/P99) + 操作量/分钟
├─ MongoDB 操作延迟 (按 collection 分组)
├─ Redis 命令延迟 + 命中率趋势
├─ Kafka Consumer Lag 趋势 (按 topic + partition)
├─ etcd 操作延迟 (RegisterService / DiscoverService)
└─ 各数据库连接池状态
```

**Dashboard 4: 业务指标 (Business Metrics)**

```
├─ 用户登录 QPS + 成功率 (成功/失败 堆叠图)
├─ 消息发送量趋势 (每分钟)
├─ 消息推送成功率 (成功/失败 占比)
├─ 推送延迟分布 (P50/P95/P99/P999)
├─ Top 10 最活跃用户 (按消息发送量)
└─ 消息类型的分布 (文本/图片/文件 饼图)
```

#### 5.3 Grafana Dashboard Provisioning

在 `docker/otel/` 下创建 Grafana 自动加载配置：

```
docker/otel/
├── grafana/
│   ├── provisioning/
│   │   ├── datasources/
│   │   │   └── datasources.yaml      # 自动配置 VictoriaMetrics + Elasticsearch + Jaeger
│   │   └── dashboards/
│   │       └── dashboards.yaml        # 自动加载 dashboards 目录
│   └── dashboards/
│       ├── im-service-overview.json
│       ├── grpc-service-details.json
│       ├── infrastructure-health.json
│       └── business-metrics.json
```

修改 `docker/otel/docker-compose.yml` 中 Grafana service，添加 volumes 和 provisioning 路径。

### 阶段六：告警规则

在 Grafana Alerting 中配置：

| 告警名称 | 条件 | 持续时间 | 严重级别 |
|---------|------|---------|---------|
| 服务不可用 | `health_check_failed > 0` | 30s | **Critical** |
| gRPC 错误率过高 | `rate(rpc_server_requests_total{status_code!="OK"}[5m]) / rate(rpc_server_requests_total[5m]) > 0.05` | 5m | Warning |
| gRPC P99 延迟过高 | `histogram_quantile(0.99, rpc_server_duration_ms) > 1000` | 5m | Warning |
| WS 连接数异常下降 | `(ws_connections_active offset 1m) - ws_connections_active > (ws_connections_active offset 1m) * 0.5` | 1m | **Critical** |
| Kafka 消费 Lag 过高 | `kafka_consumer_lag > 10000` | 5m | Warning |
| MySQL 连接失败 | `rate(db_query_duration_ms{db_system="mysql",status="error"}[5m]) > 0` | 1m | **Critical** |
| MongoDB 连接失败 | `rate(db_query_duration_ms{db_system="mongodb",status="error"}[5m]) > 0` | 1m | **Critical** |
| Redis 连接失败 | `rate(redis_command_duration_ms{status="error"}[5m]) > 0` | 1m | **Critical** |
| 消息推送延迟过高 | `histogram_quantile(0.99, msg_push_latency_ms) > 3000` | 5m | Warning |
| 磁盘空间不足 (ES) | ES 磁盘使用率 > 85% | 5m | Warning |

---

## 三、文件变更清单

| 操作 | 文件 | 说明 |
|------|------|------|
| **新增** | `include/util/otel_tracer.h` | OTel Tracer/Meter/Logger 初始化器头文件 |
| **新增** | `src/util/otel_tracer.cpp` | OTel SDK 初始化实现 |
| **新增** | `include/util/otel_metrics.h` | 指标定义头文件（所有 Counter/Histogram/Gauge 声明） |
| **新增** | `src/util/otel_metrics.cpp` | 指标注册与初始化实现 |
| **新增** | `include/util/otel_grpc_interceptor.h` | gRPC Server 拦截器头文件 |
| **新增** | `src/util/otel_grpc_interceptor.cpp` | gRPC Server 端拦截器实现（提取 traceparent、创建 Span） |
| **新增** | `include/util/otel_grpc_client_interceptor.h` | gRPC Client 端工具头文件 |
| **新增** | `src/util/otel_grpc_client_interceptor.cpp` | gRPC Client 端埋点实现（注入 traceparent） |
| **新增** | `include/util/spdlog_otel_sink.h` | spdlog 自定义 sink（日志输出到 OTLP） |
| **新增** | `src/util/spdlog_otel_sink.cpp` | spdlog → OTLP 桥接实现 |
| **新增** | `docker/otel/grafana/provisioning/datasources/datasources.yaml` | Grafana 数据源自动配置 |
| **新增** | `docker/otel/grafana/provisioning/dashboards/dashboards.yaml` | Dashboard 自动加载配置 |
| **新增** | `docker/otel/grafana/dashboards/im-service-overview.json` | Dashboard 1: 服务总览 |
| **新增** | `docker/otel/grafana/dashboards/grpc-service-details.json` | Dashboard 2: gRPC 详情 |
| **新增** | `docker/otel/grafana/dashboards/infrastructure-health.json` | Dashboard 3: 基础设施 |
| **新增** | `docker/otel/grafana/dashboards/business-metrics.json` | Dashboard 4: 业务指标 |
| **修改** | `vcpkg.json` | 添加 `opentelemetry-cpp` 依赖 |
| **修改** | `CMakeLists.txt` | 链接 OpenTelemetry C++ SDK 库 |
| **修改** | `src/main.cpp` | 启动时调用 `InitTracer()`, `InitMeter()`, `InitLogger()` |
| **修改** | `include/services/base_service.h` | `RunServer()` 注册 OTel gRPC Server 拦截器 |
| **修改** | `src/gateway/webserver.cpp` | WebSocket 消息处理添加 Span |
| **修改** | `src/gateway/ws_session.cpp` | 从 WebSocket 消息提取 traceparent + 记录指标 |
| **修改** | `src/gateway/ws_session_manager.cpp` | WebSocket 连接数 Gauge 更新 |
| **修改** | `src/messagequeue/kafkaproducer.cpp` | Kafka 生产埋点 + traceparent 注入消息头 |
| **修改** | `src/messagequeue/kafkaconsumer.cpp` | Kafka 消费埋点 + traceparent 提取 + Lag 指标 |
| **修改** | `src/services/auth_service/server.cpp` | 登录成功/失败指标 + gRPC Server Span 属性 |
| **修改** | `src/services/msg_service/server.cpp` | 消息发送计数指标 |
| **修改** | `src/services/push_service/server.cpp` | 推送延迟指标 + gRPC Client Span |
| **修改** | `src/util/mysqlconnector.cpp` | MySQL 操作 Span + 延迟 Histogram |
| **修改** | `src/util/mongoconnector.cpp` | MongoDB 操作 Span + 延迟 Histogram |
| **修改** | `src/util/redisconnector.cpp` | Redis 命令 Span + 延迟 Histogram |
| **修改** | `src/util/etcdconnector.cpp` | etcd 操作 Span |
| **修改** | `docker/otel/docker-compose.yml` | Grafana volume 挂载 provisioning + dashboards |

---

## 四、实施优先级

| 优先级 | 阶段 | 涉及内容 | 预期收益 |
|--------|------|---------|---------|
| **P0** | SDK 初始化 + gRPC 拦截器 | OTel SDK 引入、Tracer/Meter 初始化、gRPC Server/Client 拦截器 | 立即获得所有 RPC 调用链路追踪，定位分布式调用中的瓶颈 |
| **P1** | 数据库/MQ 埋点 + 业务指标 | MySQL/MongoDB/Redis/Kafka 埋点、业务指标注册 | 发现基础设施性能瓶颈，量化业务指标 |
| **P2** | spdlog 桥接 + Grafana Dashboard | 日志与 Trace 关联、4 个 Dashboard 创建 | 可视化运营数据，一站式排查问题 |
| **P3** | 告警规则 + Kibana 日志分析 | Grafana Alerting 规则、Kibana index pattern | 7×24 小时运维保障，日志全文检索 |

### 实施建议

1. **P0 可以独立执行**，不依赖后续阶段。引入 OTel SDK 并为 gRPC 添加拦截器后，即可在 Jaeger UI 中看到完整的服务间调用链路。
2. **P0 和 P1 可并行开发**，由不同开发者分别负责。
3. **P2 依赖 P0 和 P1 的数据产出**，需要等指标和链路数据上报后，Dashboard 才有意义。
4. **P3 在系统稳定运行一段时间后**（积累足够的历史数据），再根据实际流量特征调整告警阈值。

---

## 五、预期效果

实施完成后，运维/开发人员可以在 **Grafana** 中一站式监控整个 IM 服务端：

```
问题发现流程示例：

1. Grafana 告警：「gRPC P99 延迟过高」
2. 打开 Dashboard 2 (gRPC 详情) → 定位到 /MsgService/SendMessages 延迟突增
3. 点击该数据点 → 自动跳转 Jaeger → 查看该时间段 Trace 列表
4. 选择一个慢 Trace → 看到完整调用链：
   WS /msg/send (500ms) → /MsgService/SendMessages (480ms) → MongoDB insert (450ms)
5. 定位到瓶颈在 MongoDB 写入
6. 在 Kibana 中用 trace_id 搜索日志 → 查看具体写入的文档大小、collection
7. 针对 MongoDB 做索引优化或扩容
```

---

## 六、附录：关键 OTel 语义约定参考

参照 [OpenTelemetry Semantic Conventions](https://opentelemetry.io/docs/specs/semconv/)：

### RPC (gRPC)
- `rpc.system`: `"grpc"`
- `rpc.service`: 服务名 (e.g., `"AuthService"`)
- `rpc.method`: 方法名 (e.g., `"Login"`)
- `rpc.grpc.status_code`: gRPC 状态码

### Database
- `db.system`: `"mysql"` / `"mongodb"` / `"redis"`
- `db.operation`: `"SELECT"` / `"find"` / `"GET"`
- `db.collection.name`: MongoDB collection 名称
- `db.statement`: SQL 语句（可选，需脱敏）

### Messaging (Kafka)
- `messaging.system`: `"kafka"`
- `messaging.destination`: topic 名称
- `messaging.kafka.consumer.group`: 消费者组
- `messaging.kafka.partition`: 分区号
- `messaging.kafka.offset`: 偏移量

### HTTP (WebSocket 升级)
- `http.method`: `"GET"` (WebSocket 升级)
- `http.scheme`: `"ws"` / `"wss"`
- `net.peer.ip`: 客户端 IP
- `net.peer.port`: 客户端端口