# 运维与本地开发

> 状态：现状  
> 权威性：本文是「本地开发、构建、运行、测试、可观测、排障」的唯一入口  
> 最后核对：2026-11-26 @ `03ed7c2` + 工作区改造  
> 适用代码：`scripts/`、`docker/`、`src/main.cpp`  
> 组件命令以原 README 为准：[docker/README.md](../docker/README.md)、[proto/Readme.md](../proto/Readme.md)、[scripts/smoke/README.md](../scripts/smoke/README.md)。

## 1. 前置

- Linux / WSL Ubuntu 24.04（服务端只支持 Linux 构建）。
- CMake ≥3.20、Ninja、g++（C++20）。
- vcpkg 依赖已安装到 `vcpkg_installed/x64-linux`。
- Docker（可选）：etcd / Kafka / Redis / MongoDB / MySQL / OTel。
- 开发证书：`scripts/certs/{ca.crt,server.crt,server.key}`。

## 2. 构建

```bash
# 复用已有 build/（当前为 Debug + Ninja）
cmake --build build -j4

# 重新配置（CMakeLists/测试 option 变化时）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
```

构建产物：`build/libape-server.a`、`build/main`。`src/proto/*.pb.*` 是生成物，不要手改。

## 3. 启动依赖

```bash
# 交互式菜单：启动/停止 etcd、Kafka、Redis、MongoDB、MySQL、OTel
cd scripts && ./quickstart.sh

# 或逐个启动
cd docker/etcd    && ./deploy.sh standalone
cd docker/kafka   && ./deploy.sh standalone
cd docker/redis   && ./deploy.sh standalone
cd docker/mongodb && ./deploy.sh standalone
cd docker/mysql   && ./deploy.sh standalone
cd docker/otel    && ./deploy.sh start
```

MySQL 初始化：库 `ape_auth`，用户 `ape_user/ape_password`；表由服务启动时 `Initialize*` 创建/补齐。`docker/mysql/init.sql` 含遗留表，不是运行期 schema 权威。

## 4. 运行

```bash
# 单进程全启动
./build/main --role=all

# 多进程
./build/main --role=backbon
./build/main --role=auth
./build/main --role=msg
./build/main --role=push
./build/main --role=group
./build/main --role=friend
./build/main --role=conversation
./build/main --role=gateway --ws-port=6666 --push-port=50055
```

启动顺序：Backbon → Auth/Msg/Group/Friend/Conversation/GatewayPush → Kafka producer → PushService → OutboxWorker → 等待 etcd 注册（共享 30s）→ 网关。

端口见 [architecture.md](architecture.md) 或根 [README](../README.md)。

## 5. 协议生成

修改 `proto/*.proto` 后必须重新生成：

```bash
./vcpkg_installed/x64-linux/tools/protobuf/protoc \
  -I=./proto --cpp_out=./src/proto --grpc_out=./src/proto \
  --plugin=protoc-gen-grpc=./vcpkg_installed/x64-linux/tools/grpc/grpc_cpp_plugin \
  ./proto/*.proto
```

生成后编译；客户端仓库需同步 proto 并重新生成。

## 6. 测试

```bash
# 群聊集成测试（独立 MariaDB，端口 33079；会下载 deb）
bash scripts/run_group_tests.sh

# 单聊 ACK 验收（需要 backbon/auth/msg/push/gateway）
bash scripts/smoke/start_ack.sh
python3 scripts/smoke/ack_test.py --json /tmp/ack-results.json

# 多网关冒烟（破坏性：pkill build/main + 清 etcd /services/）
bash scripts/smoke/start_services.sh
python3 scripts/smoke/ws_test.py
python3 scripts/smoke/group_test.py
python3 scripts/smoke/drain_test.py
```



```bash
# 可观测单测 + 配置静态校验（不依赖中间件）
cmake -S . -B build -DAPE_OBSERVABILITY_TESTS=ON
cmake --build build --target observability_test -j8
./build/observability_test
python3 scripts/observability/validate_configs.py
```

测试策略与用例见 [testing/README.md](testing/README.md)。

## 7. 可观测

语义、指标表、日志字段、告警与处置的唯一来源：[design/observability.md](design/observability.md)。

| 能力 | 入口 |
|---|---|
| Trace | Jaeger `http://localhost:16686`；每帧 root span；同步段 parent、异步投递段 link + `im_outbox.traceparent` |
| Metric | Grafana `http://localhost:3000`；看板 `ape-overview` / `ape-gateway` / `ape-messaging` / `ape-group`；VictoriaMetrics `8428` |
| Log | Kibana `http://localhost:5601`；`APE_LOG_*` 结构化字段 + trace_id |
| Health | 网关 `--health-port=8080`（或 `HEALTH_PORT`）→ `/healthz`、`/readyz`；服务端 gRPC health |
| 校验 | `python3 scripts/observability/validate_configs.py` |
| OTel Collector | `4317`（OTLP gRPC） |

排障优先用 `account`、`type`、`requestId`、`conversationID`、`serverMsgID`、`logID`（= trace id）关联；客户端报障时先要 `logID`。

## 8. 配置

| 变量 | 缺省 | 作用 |
|---|---|---|
| `SNOWFLAKE_WORKER_ID` / `SNOWFLAKE_DATACENTER_ID` | `1`/`1` | Snowflake（群 ID 等） |
| `SERVICE_INSTANCE_ID` | `hostname:port` | etcd 实例 ID；重启需稳定 |
| `SERVICE_ADVERTISE_ADDR` | 本机非 loopback IPv4:port | 注册上报地址 |
| `GATEWAY_ROUTING` | `1` | `0` = 只广播，不读写路由 |
| `GATEWAY_DRAIN_GRACE_SECONDS` | `30` | 优雅下线等待 |
| `GATEWAY_MAX_CONNECTIONS` | `20000` | 单网关连接上限 |
| `GATEWAY_PUSH_ADDR` | `localhost:50055` | 服务发现不可用兜底 |
| `OUTBOX_WORKERS` | MySQL 池数量 | 单聊 outbox worker 数 |
| `OUTBOX_RETENTION_DAYS` / `OUTBOX_FRIEND_RETENTION_DAYS` / `OUTBOX_ACK_RETENTION_DAYS` | `7`/`7`/`1` | 完成 outbox 保留期 |
| `OUTBOX_CLEANUP_INTERVAL_MS` / `OUTBOX_CLEANUP_BATCH` / `OUTBOX_CLEANUP_MAX_BATCHES` | `600000`/`1000`/`10` | 清理节奏 |
| `APE_ALLOW_UNREGISTERED` | 未设置 | `1` = 关键服务注册失败降级继续 |
| `GROUP_TOPIC` | `group_topic` | 群聊 topic 覆盖 |
| `OTEL_SDK_DISABLED` | 未设置 | `true` 关闭 trace/metric/log 导出（保留控制台日志） |
| `OTEL_EXPORTER_OTLP_ENDPOINT` | `localhost:4317` | collector 地址，支持 `http://` / `https://` |
| `OTEL_SERVICE_NAME` | `ape-<role>` | 覆盖 `service.name` |
| `SERVICE_INSTANCE_ID` | `hostname:pid` | 实例维度（日志/指标/Jaeger 共用） |
| `OTEL_TRACES_SAMPLER_ARG` | `1.0` | 采样比例（0.0–1.0） |
| `SPDLOG_LEVEL` | `info` | `trace`/`debug`/`info`/`warn`/`error` |
| `HEALTH_PORT` | `0` | 网关健康检查端口，0 = 关闭 |

## 9. 排障

### 消息发送后对方收不到

1. 看发送方是否收到 `PENDING`/`101` 响应；确认 `im_outbox` 有 `op=1` 行。
2. 看 outbox worker：`stage` 是否到 1、`state` 是否被租约卡住、`last_error`。
3. 看 Mongo 是否有 `serverMsgID` 文档、`committed` 是否为 true。
4. 看 Kafka `msg_topic` 是否有该 `convID` 的消息；PushService 是否消费。
5. 看 `gw:user:{recvID}` 路由与 `GatewayRouter` 三态；`Unknown` 不得当离线。
6. 看接收方是否回 `106`；`im_conversation_receipts.delivered_seq` 是否推进。

### ACK 不推进/重复投递

- `delivered_seq` 只增不减；检查 `upToSeq` 是否大于 `max_seq`（会被截断）。
- 低水位 ACK 不写、不排 op=3；重复 ACK 幂等。
- 会话不存在/非参与者 ACK 是 no-op，不断连。
- 检查 `op=3` 是否被租约/重试卡住。

### 群消息不推送

- 看 `246` 是否成功、`im_group_stream.next_seq/published_seq` 是否推进。
- 看 `GroupOutboxWorker` 是否把正文写 Mongo 并发布 Kafka `group_topic`。
- 看 `GroupFanoutHandler` 是否消费、成员缓存/`joinSeq` 过滤、`PushBatchBestEffort` 结果。
- 大群只 hint；客户端需 `245` Pull，不能等推送。

### 网关路由/互踢

- `gw:user:{account}` 值是否为 `instanceId:sessionId`；TTL 300s。
- 心跳 CAS 失败会主动断开；检查是否被新实例顶替。
- 多实例 `--push-port` 不能冲突（尤其 `50056`）。
- 滚动升级必须保证新旧接口共存，或整包替换。

### 消息卡在哪一段（trace 路径）

1. 客户端响应的 `logID` 就是发送 trace id；Jaeger 搜 `logID`，应看到 `WS 101` → `MsgService/SendMessages`。
2. 用 `serverMsgID` 搜日志/`im.server_msg_id` tag，找到 `outbox.publish.op1`（投递尝试 trace）；它是新 trace，link 指回发送 trace。
3. 投递 trace 内应依次出现 `Kafka Consume msg_topic` → `PushService/PushMsg` → `GatewayPush/PushFrame`。
4. 任一段缺失：看该段指标（`ape_outbox_*`、`ape_kafka_*`、`ape_push_*`）与结构化日志字段 `last_error`/`result`。

### 启动失败

- 关键服务（msg/push/gateway push）etcd 注册失败默认退出码 1；`APE_ALLOW_UNREGISTERED=1` 可降级。
- 端口占用/证书缺失/DB 连不上会启动失败；看日志中的 `[Service]` 前缀。
- 当前 `main` 顶层 catch 返回 1；证书/端口/DB 失败路径以代码为准。

## 10. 高风险脚本

未经明确同意不要执行：

- `scripts/recompile.sh`：停止 etcd/kafka/redis/mongodb/mysql/otel 后 `make`。
- `scripts/quickstart.sh`：停止分支会影响其他本地使用者。
- `scripts/smoke/start_*.sh`：`pkill -9 -f 'build/main'` 并清空 etcd `/services/`。
- `scripts/long_conn_test.sh`：压测会打满连接/带宽，只能隔离环境跑。
- `scripts/sql/drop_legacy_groups.sql`：删除全部群表，破坏性重置。

## 11. 相关文档

- [architecture.md](architecture.md)
- [protocol.md](protocol.md)
- [implementation/schema.md](implementation/schema.md)
- [testing/README.md](testing/README.md)
- [docker/README.md](../docker/README.md)
- [proto/Readme.md](../proto/Readme.md)
- [scripts/smoke/README.md](../scripts/smoke/README.md)
