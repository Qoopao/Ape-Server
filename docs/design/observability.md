# 可观测：Trace / Metric / Log 契约

> 状态：已实施
> 权威性：本文是「可观测语义」的唯一来源：span 命名与传播、指标与标签、日志字段、告警与排障入口
> 最后核对：2026-11-26 @ 工作区可观测改造（编译 + 单测通过；端到端依赖 collector，未在本机执行）
> 适用代码：`src/om/`、`src/gateway/`、`src/services/`、`docker/otel/`、`scripts/observability/`

## 1. 目标与非目标

**目标**：任何一条消息，能用 `logID` 或 `serverMsgID` 回答“卡在哪一段”——网关、MsgService、outbox、Kafka、PushService、目标网关，以及为什么。

**非目标**：

- 不追求每个 `co_await` 都埋点；只在**边界**建 span（WS 帧、gRPC、Kafka、outbox 状态迁移）。
- 不把 `account` / `conversationID` / `serverMsgID` 做成指标标签（只进 trace/log）。
- 不引入新的可观测依赖；沿用 OTel C++ SDK 1.24 + OTLP Collector。

## 2. 运行时模型（为什么不能靠线程本地上下文）

OTel 的 `RuntimeContext` 是**线程本地**的；本项目的 handler 全部 `co_spawn` 到 Redis/MySQL executor，`co_await` 之后协程可能在另一个线程恢复。因此：

- 跨异步边界一律显式传 `ape::otel::TraceContext`（W3C traceparent 字符串）。
- 不在协程里创建长期存活的 `Scope`；`SpanHandle` 只负责 Start/End，`End()` 可在任意线程调用。
- gRPC 拦截器只做 RED 指标，不建 span、不持有 Scope；关键 handler 自己从 metadata 还原父上下文并建 server span，避免同一 RPC 两个 span。
- 日志的 trace 关联由 `APE_LOG_*` 显式携带，或由 sink 读线程本地 traceparent；不依赖 `RuntimeContext`。

## 3. Trace 模型

```text
客户端 ──WS──▶ 网关 root span（span 名 "WS <type>"，同时生成 logID）
        │ gRPC metadata: traceparent
        ▼
   MsgService/SendMessages 或 GroupService/SendGroupMessages（server span）
        │ 写 im_outbox.traceparent（与消息落库同一事务）
        ▼
   （异步，可能数秒后）OutboxWorker：
        outbox.publish.op1 / op4 —— 新 trace，link 指回受理 span
        │ Kafka value: "traceparent|serverMsgID"
        ▼
   Kafka Consume <topic>（child of publish span）
        ▼
   PushService/PushMsg → GatewayPush/PushFrame（server span）
        │ WS 104 帧带 logID
        ▼
   客户端 106 ACK（只记指标，不强制进同一条 trace）
```

规则：

1. **一帧一 span**：网关每个业务帧一个 root span，属性 `ws.type`、`ws.account`、`ws.request_id`；ping 不建 span。
2. **同步段用 parent**：gRPC client 用 `InjectTraceContext(ctx, trace)` 显式注入；server 用 metadata 还原。
3. **异步段用 link**：投递尝试是**新 trace**，`StartLinkedSpan` 通过 4 参数 `StartSpan` 传入 origin link（SDK 1.24 的 `StartSpanOptions` 无 links 字段）；同时写 `im.origin_trace_id` 属性，便于按 tag 搜索。
4. **origin 落库**：`im_outbox.traceparent`（`VARCHAR(96) NOT NULL DEFAULT ''`）保存受理 span 的 traceparent；群聊 op=4 复用同一列，不额外加 staging 列。
5. **Kafka 载体**：value 约定 `traceparent|serverMsgID`；由 `EncodeKafkaValue/DecodeKafkaValue` 统一编解码，禁止手写 `|`。
6. **logID**：`SdkWSResp.logID` 填当前帧 root span 的 32 位 trace id（101/246/245/244 响应，以及网关推送的 104 帧）；无 trace 时为空串。
7. **队列消费**：`Consumer span` 是 publish span 的 child（同一 trace），因此一次投递尝试的 trace 可以从 publish 一直看到目标网关。

span 命名：

| 位置 | 名称 | kind |
|---|---|---|
| 网关帧 | `WS <type>` | internal |
| MsgService | `MsgService/SendMessages` | server |
| GroupService | `GroupService/SendGroupMessages` | server |
| Outbox | `outbox.publish.op1` / `outbox.publish.op4` | producer |
| Kafka | `Kafka Consume <topic>` | consumer |
| PushService | `PushService/PushMsg` | server |
| 目标网关 | `GatewayPush/PushFrame` | server |

## 4. 指标契约

导出路径：SDK OTLP gRPC → Collector `prometheusremotewrite` → VictoriaMetrics。Prometheus 名称由 OTel 名称转换：`.`→`_`，counter 追加 `_total`，histogram 追加 `_milliseconds_bucket`。

标签白名单：`type`、`operation`、`result`、`reason`、`kind`、`mode`、`topic`、`partition`、`status_code`。禁止把账号/会话/消息 ID 作为标签。

| 指标（OTel 名） | 类型 | 关键标签 | 用途 |
|---|---|---|---|
| `ape.ws.connections_active` | UpDownCounter | — | 在线连接数 |
| `ape.ws.frames` | Counter | `direction,type,result` | 帧量与未知类型 |
| `ape.ws.request_duration` | Histogram(ms) | `type` | 网关处理耗时 |
| `ape.ws.auth` | Counter | `result` | 注册/登录结果 |
| `ape.ws.kicks` | Counter | `reason` | 互踢/重复登录 |
| `ape.ws.write_drops` | Counter | `reason` | 写队列满→断连 |
| `ape.ws.write_queue_frames` | UpDownCounter | — | 待发送帧总量 |
| `ape.ws.route_renew_failures` | Counter | — | 路由 CAS 失败 |
| `ape.im.send` | Counter | `kind,result` | 发送受理（single/group） |
| `ape.im.send_duration` | Histogram(ms) | `kind` | 受理耗时 |
| `ape.outbox.pending` | UpDownCounter | `operation` | 未完成 outbox 行数 |
| `ape.outbox.oldest_age` | UpDownCounter | `operation` | 最老积压年龄（秒） |
| `ape.outbox.attempts` | Counter | `operation,result` | 处理成功/失败 |
| `ape.outbox.tombstones` | Counter | `operation` | 放弃推送写 tombstone |
| `ape.kafka.produce` | Counter | `topic,result` | 生产结果 |
| `ape.kafka.produce_duration` | Histogram(ms) | `topic` | 生产确认耗时 |
| `ape.kafka.consume` | Counter | `topic,result` | 消费结果 |
| `ape.kafka.lag` | UpDownCounter | `topic,partition` | 消费 lag（librdkafka stats） |
| `ape.push.deliveries` | Counter | `result` | delivered/not_online/unknown/discarded/rejected |
| `ape.push.acks` | Counter | `kind` | ACK 会话数 |
| `ape.group.fanout.recipients` | Counter | `mode,result` | 群扇出收件人结果 |
| `ape.group.fanout.duration` | Histogram(ms) | `mode` | 群扇出耗时 |
| `rpc.server.requests` | Counter | `rpc_service,rpc_method,status_code` | gRPC RED |
| `rpc.server.duration` | Histogram(ms) | `rpc_service,rpc_method` | gRPC 延迟 |

说明：SDK ABI v1 没有同步 Gauge，`pending/oldest_age/lag/write_queue_frames` 用 UpDownCounter 维护当前值（导出为 gauge 语义）；`SetOutboxBacklog/SetKafkaLag` 内部按标签记住上次值，只发增量。

## 5. 日志契约

- 结构化事件 API：`APE_LOG_INFO("event").Field("k", v).Trace(trace);`
- 控制台一行 `[event] k=v k=v trace_id=...`；OTel LogRecord 携带同名字段，Kibana 可按字段过滤。
- 事件命名：`<domain>.<action>.<result>`，例如 `msg.send.accepted`、`outbox.publish.failed`、`group.fanout`、`ws.auth.failed`。
- 每条日志由 sink 自动补 `thread.name`、`trace_id`、`span_id`（有 traceparent 时）。
- **禁止**：消息正文、token、密码、完整 payload；热路径（每条 Kafka 消息/每次生产成功）不得用 info。
- 日志级别：默认 info，`SPDLOG_LEVEL=debug|warn|error` 可调；结构化事件优先于字符串拼接。

## 6. 采集拓扑与配置

```text
进程（OTLP gRPC 4317）→ otel-collector
  traces  → jaeger-all-in-one（ES 存储）
  metrics → prometheusremotewrite → VictoriaMetrics → Grafana
  logs    → elasticsearch → Kibana
```

| 配置 | 默认 | 说明 |
|---|---|---|
| `OTEL_SDK_DISABLED` | 未设置 | `true` 关闭导出（保留控制台日志与 no-op 指标） |
| `OTEL_EXPORTER_OTLP_ENDPOINT` | `localhost:4317` | 支持 `host:port` / `http://` / `https://` |
| `OTEL_EXPORTER_OTLP_INSECURE` | `true` | `false` 时启用 TLS |
| `OTEL_SERVICE_NAME` | 按 role | 默认 `ape-<role>`，如 `ape-gateway`、`ape-msg`、`ape-all` |
| `SERVICE_INSTANCE_ID` | `hostname:pid` | 实例维度 |
| `APE_ENV` | `dev` | `deployment.environment.name` |
| `OTEL_TRACES_SAMPLER_ARG` | `1.0` | `TraceIdRatioBased` 采样比例 |
| `SPDLOG_LEVEL` | `info` | 控制台与 OTel 日志级别 |
| `HEALTH_PORT` / `--health-port` | `0`（关闭） | 网关 `/healthz`、`/readyz` |

Collector 配置：`docker/otel/otel-collector-config.yaml`（`memory_limiter` + `batch`，logs 只导出 ES）。

## 7. 看板与告警

- 数据源：`docker/otel/grafana/provisioning/datasources/datasources.yaml`（vm/jaeger/es）。
- 看板：`docker/otel/grafana/dashboards/`
  - `ape-overview`：连接、发送 QPS、P99、outbox 年龄、lag、推送三态
  - `ape-gateway`：连接/认证/帧/写队列/踢人/路由续期
  - `ape-messaging`：发送、outbox、Kafka、推送
  - `ape-group`：群发送/扇出/Kafka lag/op4 年龄
- 告警：`docker/otel/grafana/provisioning/alerting/alert-rules.yaml`（7 条）。
  - outbox 单聊年龄 > 60s（5m）
  - outbox 群聊年龄 > 300s（5m）
  - tombstone > 0（立即）
  - Kafka lag > 10000（10m）
  - push unknown 比例 > 5%（5m）
  - 网关连接数 = 0（2m）
  - gRPC 错误率 > 1%（5m）

> 告警 provisioning 的结构由 `scripts/observability/validate_configs.py` 静态校验；本机未启动 Grafana 做运行期验证。

## 8. 告警与处置

| 告警 | 先看 | 处置 |
|---|---|---|
| outbox age（op=1） | `ape_outbox_pending{operation="1"}`、`im_outbox.last_error`、outbox worker 日志 | 看 Kafka 是否可写、Mongo 是否可写、租约是否卡住；必要时扩容 worker |
| outbox age（op=4） | 同上 + `im_group_send_staging` 孤儿 | 看群 worker、Mongo、`GROUP_PUBLISH_MAX_ATTEMPTS` |
| tombstone | `ape_outbox_tombstones_total`、`GroupOutbox` 日志 | 确认是否已写 Mongo/tombstone；按 `serverMsgID` 查 trace |
| Kafka lag | `ape_kafka_lag`、消费日志、分区数 | 扩容 PushService 消费并发/分区；确认无 offset 卡住 |
| push unknown | `ape_push_deliveries_total{result="unknown"}`、`GatewayRouter` 日志 | 检查目标网关实例注册/网络；Unknown 不得当离线 |
| 连接数归零 | 进程存活、证书、端口、`ws.auth` | 重启/回滚；看 `SERVER_DRAINING` |
| gRPC 错误率 | `rpc.server.requests{status_code!="OK"}`、按 method 下钻 | 看下游依赖与超时；必要时回滚 |

## 9. 验证

```bash
# 1) 单测（不依赖中间件）
cmake -S . -B build -DAPE_OBSERVABILITY_TESTS=ON
cmake --build build --target observability_test -j8
./build/observability_test

# 2) 配置静态校验（YAML/JSON/指标名/挂载）
python3 scripts/observability/validate_configs.py

# 3) 端到端（需 collector/jaeger/vm + 已发一条消息）
python3 scripts/observability/observability_smoke.py \
  --log-id <SdkWSResp.logID> --server-msg-id <serverMsgID>
```

手工验收路径：发一条单聊 → 取响应 `logID` → Jaeger 查该 trace（应见 `WS 101`、`MsgService/SendMessages`）→ 按 `serverMsgID` 查投递 attempt trace（应见 `outbox.publish.op1`、`Kafka Consume msg_topic`、`PushService/PushMsg`、`GatewayPush/PushFrame`）→ Grafana 看 `ape_im_send_total`、`ape_outbox_pending`、`ape_push_deliveries_total`。

## 10. 已知边界

1. 只有关键 handler 建 span（msg/群发/推送/网关推送）；其余 RPC 只有拦截器 RED 指标。
2. 客户端 gRPC 调用没有统一 client interceptor 指标；下游耗时看 server span。
3. `logID` 只保证 101/246/245/244 响应与 104 推送帧；其他类型的响应可能为空串。
4. 异步投递用 link（新 trace），不是一条超长 trace；按 `im.server_msg_id` 或 `im.origin_trace_id` 搜索。
5. ES 日志保留/ILM、tail sampling、Collector 自监控未做。
6. 告警 provisioning 需要 Grafana 10+ 的 unified alerting；本机未做运行期验证。
