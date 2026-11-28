# ape-server

ape-server 是一套面向桌面/移动 IM 客户端的 **长连接即时通信后端**：WSS 接入、账号与好友、单聊与群聊、会话与未读、消息可靠投递、多网关路由，以及基于 OpenTelemetry 的可观测性。

- 语言/标准：C++20
- 接入层：Boost.Asio + Boost.Beast（WSS、协程、每连接 strand、写队列）
- 服务层：gRPC + Protobuf，8 个 gRPC 服务 + 1 个 WSS 网关
- 存储：MySQL（关系/状态）、MongoDB（消息正文）、Redis（token/路由/缓存）
- 消息队列：Kafka（`msg_topic` 单聊、`group_topic` 群聊）
- 服务发现：etcd
- 可观测：OpenTelemetry（Trace / Metric / Log）
- 构建：CMake + vcpkg（`x64-linux`）

## 快速开始

```bash
# 1) 启动依赖（Docker：etcd / Kafka / Redis / MongoDB / MySQL / OTel）
cd scripts && ./quickstart.sh

# 2) 生成开发证书（首次）
bash scripts/generate_cert.sh

# 3) 构建（仓库已有 build/ 缓存时可直接编译）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4

# 4) 单进程启动（网关 + 全部服务）
./build/main --role=all
```

多进程模式：

```bash
./build/main --role=backbon
./build/main --role=auth
./build/main --role=msg
./build/main --role=push
./build/main --role=group
./build/main --role=friend
./build/main --role=conversation
./build/main --role=gateway --ws-port=6666 --push-port=50055
```

> 网关角色必须在同一进程内同时运行 `WebServer` 与 `GatewayPushService`；多网关实例的 `--push-port` 不能与 `GroupService` 的 `50056` 冲突。

## 端口

| 服务 | 端口 |
|---|---|
| WSS 网关 | 6666 |
| AuthService | 50051 |
| BackbonService | 50052 |
| MsgService | 50053 |
| PushService | 50054 |
| GatewayPushService | 50055 |
| GroupService | 50056 |
| FriendService | 50057 |
| ConversationService | 50058 |
| MySQL / Redis / MongoDB | 3306 / 6379 / 27017 |
| Kafka / etcd | 29092 / 2379 |
| OTel Collector | 4317 |

## 当前状态

- ✅ 单聊：发送幂等、每会话稠密 `seq`、MySQL outbox、Mongo 正文、Kafka → Push、按会话 ACK 水位、登录按会话同步。
- ✅ 群聊 v3：Mongo 单副本、`im_group_stream` 稠密 seq、outbox `op=4`、Kafka `group_topic` 扇出、`210/245/246`、无 ACK。
- ✅ 好友 6 个操作、会话列表/已读/置顶/免打扰/隐藏、多网关路由与互踢、优雅下线。
- ✅ OTel Trace/Metric/Log：每帧 root span、outbox→Kafka→Push 投递链、结构化日志、`logID` 反查；语义见 [docs/design/observability.md](docs/design/observability.md)。
- ⬜ 消息撤回/删除/清空、富媒体、多端同时在线、限流、服务端主动心跳、令牌吊销/续期、Mongo TTL、服务端镜像与 CI。

完整矩阵见 [docs/implementation/status.md](docs/implementation/status.md)。

## 文档

- [docs/README.md](docs/README.md)：文档地图、状态图例、权威性与阅读路径。
- [docs/design/observability.md](docs/design/observability.md)：trace/metric/log 契约、看板、告警与排障。
- [docs/architecture.md](docs/architecture.md)：系统总览、进程/端口、消息链路。
- [docs/protocol.md](docs/protocol.md)：WebSocket type 码、错误码、超时、兼容规则。
- [docs/requirements.md](docs/requirements.md)：产品范围、角色、场景、非功能需求。
- [docs/design/](docs/design/)：单聊可靠性、网关与多实例、群聊 v3、账号/好友/会话设计。
- [docs/implementation/](docs/implementation/)：功能状态、schema、待办、改造记录。
- [docs/testing/](docs/testing/)：测试策略、单聊/群聊用例与结果。
- [docs/operations.md](docs/operations.md)：本地开发、部署、可观测、排障。
- [docs/notes/interview-notes.md](docs/notes/interview-notes.md)：非权威的技术选型与面试复盘。
- [docs/archive/](docs/archive/)：历史审计与废弃方案，只读。
- [agent.md](agent.md)：Agent 工作规约。

## 约定

- `proto/*.proto` 是唯一协议契约源；生成物在 `src/proto/`，不要手改。
- `build/`、`vcpkg_installed/`、`.cache/` 是生成物，不提交。
- 当前工作区可能包含尚未提交的多网关/群聊改造；不要用 `git reset --hard` / `git clean -fd` 清理。
- 高风险脚本（`scripts/recompile.sh`、`scripts/quickstart.sh`、`scripts/smoke/start_*.sh`）会停服务、杀进程或清 etcd，执行前先读脚本。
