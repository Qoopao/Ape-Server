# 系统架构

> 状态：现状  
> 权威性：本文是「系统总览、进程/端口、分层与消息链路」的唯一来源  
> 最后核对：2026-11-26 @ `03ed7c2` + 工作区多网关/群聊改造  
> 适用代码：`src/main.cpp`、`src/gateway/`、`src/services/`、`src/storage/`

## 1. 定位

ape-server 是 IM 后端：客户端通过 WSS 建立长连接，网关完成认证、协议分发、在线推送与离线同步；业务服务通过 gRPC 处理账号、好友、会话、单聊、群聊；消息可靠性由 MySQL outbox + MongoDB 正文 + Kafka 投递 + ACK 水位保证。

一句话：**网关有状态、服务无状态、异步靠 outbox、送达靠 ACK 水位、路由只影响性能。**

## 2. 运行形态与角色

进程角色由 `--role` 决定，缺省 `all`：

| 角色 | 默认监听 | 职责 |
|---|---|---|
| `BackbonService` | `0.0.0.0:50052` | etcd 注册/发现；自身不注册 |
| `AuthService` | `0.0.0.0:50051` | 注册、登录、令牌校验 |
| `MsgService` | `0.0.0.0:50053` | 单聊受理、历史、outbox worker |
| `PushService` | `0.0.0.0:50054` | 单聊 Kafka 消费、在线投递、ACK；群聊 fanout 消费 |
| `GatewayPushService` | `0.0.0.0:50055` | 网关 RPC 门面：`PushFrame` / `PushBatch` / `IsOnline` / `KickUser` |
| `GroupService` | `0.0.0.0:50056` | 群命令、群发送/历史/同步、群 outbox worker |
| `FriendService` | `0.0.0.0:50057` | 好友 6 个操作 |
| `ConversationService` | `0.0.0.0:50058` | 会话列表/已读/设置/隐藏 |
| `WebServer`（网关） | `0.0.0.0:6666`（WSS） | TLS/WSS、认证、协议分发、离线同步 |

- `--role=all`：单进程启动全部服务，作为兼容模式/总回滚开关。
- 网关角色必须在同一进程内启动 `WebServer` 与 `GatewayPushService`，因为推送要操作本进程的 `WSSessionManager`。
- 多网关实例的 `--push-port` 不能与 `GroupService` 的 `50056` 冲突。

## 3. 分层与组件

```text
客户端 ──WSS──▶ 网关（有状态：连接表 + 写队列）
                    │ gRPC（无状态）
                    ▼
      Auth / Msg / Push / Group / Friend / Conversation / Backbon
                    │
      ┌─────────────┼──────────────┬─────────────┐
      ▼             ▼              ▼             ▼
    MySQL         MongoDB        Redis          Kafka
 （关系/状态）  （消息正文）  （token/路由/缓存）（异步投递）
```

| 层 | 组件 | 边界 |
|---|---|---|
| 接入 | `WebServer`、`WSSession`、`WSSessionManager` | 只有网关进程可访问连接表 |
| 服务 | `BaseServiceServer` + 各 `*ServiceImpl` | 无状态；通过 etcd 注册；通过 gRPC 调用 |
| 路由 | `GatewayRouter` | 非网关进程共享；三态 `Delivered/NotOnline/Unknown` |
| 存储 | `MySQLHandler`、`MongoHandler`、`RedisHandler`、connectors | 数据访问；业务规则应逐步上移 service 层 |
| 消息队列 | `KafkaProducer` / `KafkaConsumer` | 单聊 `msg_topic`；群聊 `group_topic` |
| 可观测 | `src/om/` | Trace / Metric / Log、gRPC 拦截器、traceparent 透传 |

## 4. 单聊消息链路

```text
客户端 type=101
  → 网关：覆写 sendID、建 OTel root span
  → MsgService.SendMessages
      ├─ 校验字段；服务端权威生成 convID/serverMsgID/seq/sendTime
      ├─ MySQL 单事务：lockPair → 好友校验 → 删除水位 → 幂等查/插 im_outbox
      └─ 轮询 outbox 等待落库（PENDING 时后续补 101 响应）
  → OutboxWorker
      ├─ stage 0→1：Mongo 存正文(committed=false) + 更新会话 + stage=1
      └─ 发布：Mongo committed=true → 无 ACK 水位则 Redis 缓存 + Kafka(msg_topic, key=convID)
  → PushService（消费 msg_topic）
      ├─ 取正文、MessageDispatchDone 幂等检查、好友/删除水位二次校验
      ├─ GatewayRouter.PushFrame(104) → 目标网关
      └─ 离线/Unknown：保留在 Mongo，登录时按 cursor 补投
  → 客户端 ACK type=106（按会话 upToSeq）
      → PushService：im_conversation_receipts.delivered_seq=GREATEST + 排 op=3 outbox
      → OutboxWorker：Mongo status=2（按水位批量）
```

关键事实：

- 单聊 `seq` 是**每会话稠密自增**（`im_conversation_seq`），在 `QueueSingleMessage` 的 MySQL 事务内分配；不再使用 Snowflake。
- `serverMsgID` = UUID v4；`clientMsgID` 幂等键 = `im_outbox.dedupe_key = "{sendID}:{clientMsgID}"`。
- ACK 是每会话连续水位，不是逐条回执、不是已读；无响应帧。
- 登录同步取最近 50 个会话 × 每会话最近 20 条，不等待 ACK；`209` 返回 `SyncSummary`。
- 同步/Pull 不按 Mongo `status` 过滤；ACK 提前标 `status=2` 后仍可按旧 cursor 拉到。

## 5. 群聊消息链路（v3，已实现）

```text
客户端 246 SendGroupMessageReq
  → 网关：覆写 actor → GroupService.SendGroupMessages
      ├─ MySQL 事务：成员行锁 → im_group_stream.next_seq+1 → staging 正文 + outbox(op=4)
      └─ 提交即返回 MsgData{seq,serverMsgID}
  → GroupOutboxWorker
      ├─ stage 0→1：读 staging → Mongo 存正文(committed=false) → 删 staging
      └─ 发布：Mongo committed=true → published_seq=GREATEST → Kafka(group_topic, key=groupID)
  → PushService GroupFanout
      ├─ 读 Mongo/成员/stream → 小群内联 251，大群只 hint
      └─ GatewayRouter.PushBatchBestEffort(251)
  → 客户端 245 GetGroupChatHistory（按 cursor 补洞/翻页）
  → 登录/重连：单聊 209 后，群聊 251（小群内联）+ 210 GroupSyncSummary
```

关键事实：

- 群消息在 Mongo 只有**一份**（`convID=groupID`、`isGroupMsg=true`、`recvID=0`、`sessionType=2`）。
- 群内 `seq` 由 `im_group_stream.next_seq` 单行分配，严格 +1；`published_seq` 是已发布最大 seq。
- 群聊**没有 ACK**；未读由客户端本地维护；服务端不保存群已读/送达状态。
- 旧 `250 Send(11)/Pull(12)/Ack(13)/Read(14)` 已废弃，返回 `UNSUPPORTED_OPERATION`；`250` 只保留成员/元数据命令。
- 老库需要执行 `scripts/sql/drop_legacy_groups.sql` 或由 `InitializeGroups` 检测后重建；`im_group_messages` 不再使用。

详细设计见 [design/group-chat.md](design/group-chat.md)。

## 6. 存储职责

| 存储 | 负责 | 不负责 |
|---|---|---|
| MySQL `ape_auth` | 账号、好友、会话、单聊 seq/outbox/ACK 水位、群元数据/stream/成员/outbox | 消息正文（单聊/群聊正文都在 Mongo） |
| MongoDB `IM-System.msg` | 单聊/群聊消息正文、历史范围读、投递状态 | 业务状态机、幂等键 |
| Redis | token、`gw:user:{account}` 路由、`msg:{serverMsgID}` 缓存 | 在线/送达的正确性判据 |
| Kafka | 单聊 `msg_topic`、群聊 `group_topic` 异步投递 | 消息权威存储 |
| etcd | 服务注册 `/services/{Service}/{instanceId}` | 业务数据 |

## 7. 多网关与在线判定

- 每个网关实例注册到 `/services/GatewayPushService/{instanceId}`，value 为 JSON（`instance_id`/`address`/`methods`）。
- 登录写 `gw:user:{account} = "{instanceId}:{sessionId}"`，TTL 300s；心跳用 Lua CAS 续期；续期失败说明被顶替，连接主动断开。
- 推送时 `GatewayRouter` 先按路由表找实例；明确失败/不可达时广播兜底。
- 三态：`Delivered` / `NotOnline` / `Unknown`；`Unknown` 不得当离线确认分发。
- **路由只影响性能，不影响正确性**；所有实例都明确否认才算离线。
- 优雅下线：停注册 → 关 acceptor → 推 `SERVER_DRAINING` → 停 CAS 续期 → grace → 强制关闭 → 停 IO。

## 8. 并发模型

| 组件 | 模型 |
|---|---|
| 网关 accept | 独立单线程 `_acceptor_ioc` |
| 网关业务 | `IOC_Pool` 4 个 `io_context` × 1 线程；每连接 `make_strand` 后 round-robin 绑定 |
| 网关写 | 每连接一个 `channel`（容量 256），`doDrainWriteQueue` 串行消费；认证后所有出站帧走 `queuedWrite` |
| 跨连接共享 | `WSSessionManager` 用 `shared_mutex` |
| gRPC 服务 | 每服务 1 线程 + 协程化 handler（`BaseServiceServer`） |
| 存储访问 | Redis 4 worker；MySQL 2 worker（写 pool0 / 读 pool1）；Mongo 4 worker |
| Kafka 消费 | 按 `(topic, partition)` 的 lane 串行处理 |

协程/strand 上禁止同步阻塞 IO；gRPC 通过 `ape::grpc_util::GrpcAwait` 桥接；`co_spawn` 必须有异常处理。

## 9. 平台能力

- **etcd 注册**：`BaseServiceServer` 负责 500ms→8s 退避重试、30s 看门狗、优雅注销；关键服务（msg/push/gateway push）注册失败默认退出码 1，`APE_ALLOW_UNREGISTERED=1` 可降级。
- **后台任务**：单聊 outbox worker（2+ 协程，可配）、outbox 清理、群 outbox worker、群 fanout consumer、etcd KeepAlive。
- **可观测**：语义与配置见 [design/observability.md](design/observability.md)。每帧一个 root span；traceparent 经 gRPC metadata 显式传递、经 `im_outbox.traceparent` 落库后随 Kafka value 传到推送段；指标覆盖网关/消息/outbox/Kafka/推送/群扇出；结构化日志可用 `logID` 反查 trace。
- **健康检查**：各 gRPC 服务由 `grpc::EnableDefaultHealthCheckService(true)` 提供 `grpc.health.v1`；网关可用 `--health-port` / `HEALTH_PORT` 开启 `/healthz` 与 `/readyz`（draining 时返回 503）。

## 10. 设计注记

来自早期设计笔记，保留作为选型背景：

- 网关用独立 `io_context` 处理监听，业务通过 least-conn 分发到 `IOC_Pool`；多线程 run 单个 `io_context` 会有惊群和锁竞争。
- gRPC unary 异步与协程结合：先返回 reactor，协程恢复时调用 `Finish()`，避免回调地狱。
- Redis/MySQL/MongoDB 驱动本身不提供多线程：Redis 一个线程对应一个连接；MySQL/Mongo 一个线程对应一个连接池。

## 11. 相关文档

- 协议契约：[protocol.md](protocol.md)
- 单聊可靠性：[design/messaging.md](design/messaging.md)
- 网关与多实例：[design/gateway.md](design/gateway.md)
- 群聊 v3：[design/group-chat.md](design/group-chat.md)
- 账号/好友/会话：[design/social.md](design/social.md)
- 数据模型：[implementation/schema.md](implementation/schema.md)
- 功能状态：[implementation/status.md](implementation/status.md)
