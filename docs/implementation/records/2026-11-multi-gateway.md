# 多网关改造实施记录（2026-11）

> 状态：历史实施记录  
> 权威性：本文是「多网关改造实际落点、与方案偏差、验证结果」的唯一来源  
> 最后核对：2026-11-26 @ `03ed7c2` + 工作区改造  
> 适用代码：src/gateway/、src/services/gateway_router.cpp、src/services/gateway_push_service/

| 项目 | 内容 |
|---|---|
| 依据 | `docs/design/gateway.md`（v1.0，含第 9/10 章） |
| 实施范围 | ape-server 服务端（PR-1 ~ PR-6、PR-8）；PR-7 客户端在独立仓库，未涉及 |
| 验证 | 本机 Redis/etcd/MySQL/MongoDB 起两个网关实例跑通冒烟测试，见文末 |

本文件只记录"实际做了什么、与方案有什么偏差、怎么验证"，设计理由仍以方案原文为准。

---

## 1. 改动总览（按 PR）

| PR | 状态 | 关键落点 |
|---|---|---|
| PR-1 进程拆分与实例化注册 | ✅ | `src/main.cpp`（`--role`）、`base_service.h`（instance_id + 真实地址）、`backbon_service`（`/services/{svc}/{id}`） |
| PR-2 推送接口泛化 | ✅ | `gateway_push.proto`、`gateway_push_service/*`、`push_service`、`group_service`、`outbox_worker` |
| PR-3 路由表 + 跨实例互踢 | ✅ | `util/gateway_route.h`、`ws_session.cpp`（写路由 / CAS 续期 / KICKED）、`ws_session_manager.cpp` |
| PR-4 GatewayRouter 与三态 | ✅ | 新增 `include/services/gateway_router.h`、`src/services/gateway_router.cpp`；三类调用方全部改走它 |
| PR-5 群投递反转 | ✅ | `group_service/server.cpp`、`mysqlhandler.cpp`（`QueryGroupsWithPending` / `GroupActiveMembers` / `GroupNotice`） |
| PR-6 优雅下线 + 连接上限 | ✅（指标待补） | `webserver.cpp`（DRAINING/grace）、`SERVER_BUSY`、`GATEWAY_MAX_CONNECTIONS` |
| PR-8 离线同步（按会话） | ✅ | `ws_session.cpp::syncOffline`、`sdkws.proto`（cursors/SyncSummary）、`mongohandler`（`LatestAfterMessagesAsync`） |

一句话：**网关可以起 N 个实例，任意一个挂掉不影响其他人；非网关进程不再直接摸 `WSSessionManager`；离线同步改成了有界快照。**

---

## 2. 关键实现点

### 2.1 实例身份与注册（PR-1）

- `include/util/instance_id.h` + `src/util/instance_id.cpp`：
  - 实例 ID = `SERVICE_INSTANCE_ID`，缺省 `hostname:<监听端口>`；
  - 上报地址 = `SERVICE_ADVERTISE_ADDR`，缺省取本机第一块非 loopback IPv4（优先私网段）。
- `BaseServiceServer::EnableEtcdRegistration(backbon, methods, instance_id = "")`：
  注册时带 `instance_id`，上报 `AdvertiseAddress(listen_address_)`，**不再是 `0.0.0.0`**。
- etcd key：`/services/{ServiceName}/{instanceId}`，value 为 JSON：
  ```json
  {"instance_id":"gw-1","address":"10.0.0.5:50055","methods":["PushFrame","PushBatch","IsOnline","KickUser"]}
  ```
- `GetService` 只返回实例列表（`instances`：`instance_id` + `address`）与方法集合；
  不再有 `ipport` 列表、旧单实例 key 或逗号分隔 value 的兼容分支（旧字段已直接删除，proto 字段号已重排）。
- 删除 Redis 的 `/services/{service}` methods 镜像与 `CheckUserOnline` 的负缓存。
- `--role`：`all|backbon|gateway|auth|msg|push|group|friend|conversation`，
  缺省 `all`；`all` 仍是单进程全启动（总回滚开关）。

### 2.1.1 注册的可靠性

- 注册失败会按 500ms → 8s 指数退避**重试**，直到成功或进程停止；
- `WaitForEtcdRegistration` 在服务启动失败（端口绑定失败）时立即失败，不再白等 30s；
- 每个服务有一个 30s 周期的**注册看门狗**：发现 etcd 里自己的实例消失（etcd 重启、租约丢失）就重新注册；
- `--role=all` 用**共享的 30s 截止时间**等待全部服务，而不是逐个等 30s（最坏 210s）；
- `msg/push/gateway push` 视为关键服务，注册失败默认退出码 1；`APE_ALLOW_UNREGISTERED=1` 可降级；
- 支持 `DeferRegistration()/MarkReady()`：把注册推迟到子类自身初始化完成之后。

### 2.2 推送接口与路由（PR-2/3/4）

- `gateway_push.proto` 删除 `PushToUser`，新增：
  `PushFrame` / `PushBatch` / `IsOnline` / `KickUser`，响应都带 `instance_id` 便于排障。
- `GatewayRouter`（新增）：
  - `Resolve()` 读 `gw:user:{account}`，按最后一个 `:` 切分实例 ID 与会话 UUID；
  - `PushFrame`：先打解析出的实例；明确 false 或 RPC 失败则**广播兜底**（跳过已问过的实例）；
  - `PushBatch`：`MGET` 批量读路由 → 按实例分组，每组一次 `PushBatch`；未命中/未投递的账号再广播一次；
  - 三态：`Delivered` / `NotOnline` / `Unknown`；`Unknown` 由调用方决定重试；
  - 单次 RPC 截止 300ms；广播用并发任务 + 完成计数通道，整体上界≈300ms。
- 路由表：`gw:user:{account} = "{instanceId}:{sessionId}"`，TTL 300s。
  - 登录：读旧路由 → 跨实例 `KickUser`（best-effort）→ `SET EX 300` → 本地注册（同实例踢旧连接）；
  - 心跳：Lua CAS 续期，返回 0 则发 `KICKED` 并断开；Redis 异常只跳过本次续期，不放大成集体掉线；
  - 断线：**不删 key**，靠 TTL 过期。
- `PushService`：删除 Redis 在线布尔闸门；`Delivered` 等 ACK、`NotOnline` 确认分发、`Unknown` 返回 `UNAVAILABLE`（Kafka 不提交 offset）。
- `push.proto` 增加 `PushDecision`（`PUSH_DISCARDED/PUSH_DELIVERED/PUSH_NOT_ONLINE`），
  让消费者知道"该不该确认分发"——这是方案里隐含但没写进 proto 的一环。

### 2.3 群投递反转（PR-5）

- 每 2s tick：`QueryGroupsWithPending(200)` → `GroupNotice(group)` → `GroupActiveMembers` →
  `router.PushBatch(members, 251, notice)`；
- `GroupNotice`：`lastSeq` + 成员最大 `epoch`；小群（`member_count<=200`）内联最近 20 条，大群只带 `lastSeq`（Q-5 不变）；
- `offlineCache`：`NotOnline` 成员 30s 内不再重复问；`Delivered` 清除；`Unknown` 只记日志、下 tick 重试；
- 群命令后的 `Notify` 改为推送 `{groupID, refresh:true}`（不带正文，避免重复投递）。
- 新增 SQL：
  - `QueryGroupsWithPending`：`last_seq > COALESCE(MIN(ack_seq), last_seq)` 且未解散；
  - `GroupActiveMembers`、`GroupNotice`。

### 2.3.1 单聊消息序号（seq）

- 单聊 seq 改为**每个会话一个稠密自增计数器**（新表 `im_conversation_seq`），双向共用、严格 +1；
- 分配在 `QueueSingleMessage` 的同一个 MySQL 事务里，与 outbox 插入同生共死；同一 `clientMsgID` 幂等，不会重复分配；
- `friend_chat_deletions.deleted_seq` 改为同一 seq 空间的水位（不再用 Snowflake），删除好友后的新消息 = 水位+1；
- Snowflake 不再参与单聊 seq（仍用于群 ID）；因此 `hasGap = firstSeq > from + 1` 成立；
- **不做历史迁移**：已有单聊历史与计数器需要清空/重建（`im_outbox`、`im_conversations`、`im_conversation_seq`、
  `friend_chat_deletions`），否则旧 Snowflake seq 会和新的 1,2,3… 冲突；
- 延迟 101 回执改为通过 `PushFrameReq.request_id/error_code` 透传，`data` 只放 `SendMessageResp`，
  不再把整个 `SdkWSResp` 塞进 `data`（双层包装）。

### 2.3.2 单聊 ACK（按会话水位）

- `type=106` 的 `AckReq` 改为 `repeated ConversationAck conversations`（`conversationID + upToSeq`），
  客户端只上报每个会话已处理到的水位；
- 网关一次 RPC `PushService.AckConversations` 处理整帧，不再逐条 `serverMsgID` 调用；
- ACK 是 **fire-and-forget**，不返回响应帧；`upToSeq` 超过 `max_seq` 时服务端静默截断，
  客户端 cursor 只依赖本地持久化，不依赖 ACK 结果；
- 客户端自行检测 seq 缺口并通过 244 主动补拉；服务端同步/Pull 不按 `status` 过滤；
- 采用 A 方案：不改变写入顺序、不做可见水位/写侧门卫；客户端视为可信，
  契约后续版本再强制；Pull 返回 `minSeq/deletedSeq` 为规划项；
- PushService 写入 `im_conversation_receipts(owner, conversation, delivered_seq)`（`GREATEST` 只增不减），
  并生成一条 `op=3` outbox 任务；
- `op=3` 用 `seq <= upToSeq` 批量把 Mongo 中该会话的消息置 `status=2`，不再逐条清理；
- `op=1` 的完成条件与 `MessageDispatchDone` 都改为检查该会话水位 `delivered_seq >= seq`；
- 过期 ACK（会话已删除/非参与者）按 **no-op 成功**处理：不写回执、不生成 `op=3`、不断连；
- 删除旧的 `im_message_receipts`、`AckMsg`、`AddPendingOfflineAck`，以及 `SYNC_V2=0` 的旧同步/等 ACK 模型；
- Redis 消息缓存改为随 TTL（7 天）过期，ACK 不再逐条删 `msg:{id}`。

### 2.3.3 网关入口 type 码

注册/登录不再复用业务 type：

| type | 方向 | 含义 |
|---|---|---|
| 0 | C→S / S→C | 心跳 ping / pong |
| 90 | C→S | 注册 |
| 91 | C→S | 登录 |
| 101 | C→S | 单聊发送（认证后） |
| 106 | C→S | 按会话水位 ACK |
| 104 | S→C | 在线消息推送 |
| 200 / 209 | S→C | 离线单条 / 登录同步完成 |
| 240–242、244、250–251 | 双向 | 会话、拉取、群相关命令 |

### 2.3.4 Outbox 清理（第二层策略，规划）

- 只清理 `state IN (2,3)` 的任务，`state=0/1` 永不删除；
- 保留期：op=1 7 天、op=2 7 天、op=3 1 天；
- 按 `operation + updated_at + LIMIT` 分批删除，建议每批 1000、每轮最多 10 批；
- 需要索引 `outbox_retention(state,operation,updated_at,id)`；
- 规划为 MsgService 后台线程，默认每 10 分钟执行；
- 7 天是 `clientMsgID` 幂等窗口；超期重试会重新分配 seq；
- 本策略不清理 payload（完成行仍占 `MEDIUMTEXT` 空间）；
- **当前未实现**，仅作为后续实施项。

### 2.4 登录同步（PR-8）

- `SdkWSReq` 追加 `repeated ConversationCursor cursors = 9`（旧 `lastMsgSeq` 不再读取）；
- `SdkWSResp.type=209` 的 `data` 填 `SyncSummary`（总数/同步数/推送数/hasMore/每会话区间与 hasGap）；
- `errorCode`：`0` / `PARTIAL`（脏数据或 gap）/ `SYNC_ERROR`；
- 流程：`ListRecentConversations(account, 50)` → 每会话 `LatestAfterMessagesAsync(owner, convID, cursor, 20)`
  → 过滤脏数据 → 按会话分轮（≤50 帧）入写队列 → **不等 ACK**；
- 写队列维护深度计数，轮间 `waitWriteQueueBelow(128, 1s)`；
- ACK 处理：失败改为 `co_await onDisconnect()`（不再裸 `close()`），补投统计只认 `ackType=2`；
- `SYNC_V2=0` 可回退到旧的"全量扫描 + 等 ACK"实现（旧代码保留）。

### 2.5 优雅下线（PR-6）

`WebServer::stop()` 顺序固定：

1. `drain_handler_` → 先停 `GatewayPushService`（内部从 etcd 注销）；
2. 关闭 acceptor；
3. 向所有本地 session 推 `SERVER_DRAINING`；
4. 心跳停止 CAS 续期（`WSSessionManager::draining()`）；
5. 等待 `GATEWAY_DRAIN_GRACE_SECONDS`（缺省 30s）；
6. 强制关闭剩余连接；
7. 停 IO 线程。

连接上限：`GATEWAY_MAX_CONNECTIONS`（缺省 20000），超限回 `SERVER_BUSY`。

---

## 3. 开关与配置

| 变量 | 缺省 | 作用 |
|---|---|---|
| `SERVICE_INSTANCE_ID` | `hostname:port` | 实例 ID（重启需保持稳定） |
| `SERVICE_ADVERTISE_ADDR` | 本机私网 IPv4:port | 注册上报地址 |
| `GATEWAY_ROUTING` | `1` | `0` = 不读写路由表，投递只走广播（V-9） |
| `SYNC_V2` | `1` | `0` = 旧的离线同步模型 |
| `GATEWAY_DRAIN_GRACE_SECONDS` | `30` | 下线 grace |
| `GATEWAY_MAX_CONNECTIONS` | `20000` | 单实例连接上限 |
| `OUTBOX_WORKERS` | MySQL 连接池数量 | 单聊 Outbox 的 worker 数；worker i 绑定 MySQL pool i，上限为池数量 |
| `OUTBOX_RETENTION_DAYS` | 7（规划） | op=1 完成/取消行保留天数 |
| `OUTBOX_FRIEND_RETENTION_DAYS` | 7（规划） | op=2 保留天数 |
| `OUTBOX_ACK_RETENTION_DAYS` | 1（规划） | op=3 保留天数 |
| `OUTBOX_CLEANUP_BATCH` | 1000（规划） | 单次 DELETE LIMIT |
| `OUTBOX_CLEANUP_MAX_BATCHES` | 10（规划） | 每轮每 operation 最多批数 |
| `OUTBOX_CLEANUP_INTERVAL_MS` | 600000（规划） | 清理间隔 |
| `APE_ALLOW_UNREGISTERED` | 未设置 | `1` = 关键服务（msg/push/gateway push）注册失败时降级为告警继续，而不是退出 |
| `GATEWAY_PUSH_ADDR` | `localhost:50055` | 服务发现不可用时的兜底网关地址 |

命令行：`--role`、`--ws-port`、`--push-port`、`--backbon=<host:port>`。

`--backbon` 是服务注册/发现的**唯一入口**：

- 客户端角色（auth/msg/push/group/gateway/...）连这个地址；
- `backbon`/`all` 角色从里面取端口并 bind `0.0.0.0`（host 部分被忽略）；
- 格式非法（缺端口/端口为 0/越界/非数字）**直接退出码 2**，不进入初始化；
- 不再有"改端口自动改连接地址"的隐式联动，也不存在两个参数配错的可能。

---

## 4. 与方案的偏差（需要在评审里确认）

| # | 方案原文 | 实际实现 | 原因 |
|---|---|---|---|
| 1 | 实例地址通过 **etcd 前缀订阅** 更新 | `GatewayRouter` 每 2s（或首次）通过 `GetService` 刷新 | etcd-cpp-apiv3 的 watch 需独立线程/回调，先用"短周期拉取"达到同等效果：新实例 ≤2s 可见，旧实例在路由 TTL+广播兜底内不影响正确性 |
| 2 | 广播整体截止 500ms 的独立定时器 | 每个 RPC 300ms 截止 + 并发等待全部 | 10 个实例最坏≈300ms，已低于 500ms；不额外加定时器 |
| 3 | PR-6 OTel 指标带 `instance_id` 维度 | 沿用现有指标，未加维度 | 指标改造独立于投递正确性，建议单独 PR |
| 4 | 群 `Notice` 按 actor 生成 | 改为按群生成（`MIN(ack_seq)` 作为内联起点） | 一次 `PushBatch` 只能带一份 payload；客户端按消息 ID 去重 |
| 5 | PR-7 客户端重连 | 未实现 | 客户端在 IM-client 仓库，不在本仓库范围 |

补充提醒：`GroupService` 固定占用 **50056**，第二个网关实例的 `--push-port` 不要用 50056
（本仓库冒烟脚本用 50075）。GatewayPushService 与 GroupService 必须显式分配不同端口。

---

## 5. 验证

### 5.1 静态检查（对应方案 PR-2 验收命令）

```bash
# 非网关进程不访问 WSSessionManager：仅命中注释
grep -rn "WSSessionManager" src/ include/ | grep -v '^src/gateway/' \
  | grep -v '^src/services/gateway_push_service/' | grep -v '^include/services/gateway_push_service/' \
  | grep -v '^include/gateway/'
# 旧接口已删除
grep -rn "PushToUser" proto/ src/ include/ | grep -v '\.pb\.'      # 无输出
# user:{account}:online 写入点已删除
grep -rn ':online' src/ include/                                    # 仅注释/无关 key
```

### 5.2 运行时冒烟（`scripts/smoke/`）

环境：本机 Redis(6379) + etcd(2379) + MySQL(3306) + MongoDB(27017)，
起 `backbon`、`auth`、`group` 与两个网关实例（`gw-1` ws=6666/push=50055，`gw-2` ws=6667/push=50075）。

```bash
python3 scripts/smoke/gen_proto.py
bash scripts/smoke/start_services.sh
python3 scripts/smoke/ws_test.py      # 注册/登录/209/路由/心跳 CAS/跨实例互踢/etcd 注册
python3 scripts/smoke/group_test.py   # 群消息跨实例扇出（251 到两个网关）
python3 scripts/smoke/drain_test.py   # SIGTERM -> SERVER_DRAINING + etcd 注销 < 2s
bash scripts/smoke/start_all.sh
python3 scripts/smoke/all_test.py     # --role=all 兼容模式
```

实际结果（全部通过）：

- 登录后 `gw:user:{account}` = `gw-1:<uuid>`；同账号在 gw-2 登录后变 `gw-2:<uuid>`，
  gw-1 旧连接收到 `errorCode=KICKED`；
- 篡改路由为 `gw-9:fake` 后，gw-2 在下一次心跳自杀（CAS 生效）；
- 两个网关的 etcd key 分别为 `/services/GatewayPushService/gw-1`、`/gw-2`，
  value 为 JSON 且地址不是 `0.0.0.0`；
- A/B 分连两个网关，群消息双方都收到 `type=251`（`PushBatch` 跨实例扇出）；
- `SIGTERM gw-2`：客户端收到 `SERVER_DRAINING`，etcd key 2s 内消失，grace 后进程退出；
- `--role=all` 下注册/登录/209 正常，路由值为 `hostname:50055:<uuid>`。

冒烟测试覆盖不到的部分：Kafka 链路下的 `Unknown -> 不提交 offset`、Outbox 延迟回执 101、
大群（>200 人）只推 `lastSeq` 的分支——这些需要 Kafka/Mongo 完整链路，建议按方案 §7 验收矩阵做集成验证。

---

## 6. 后续建议

1. 补 PR-6 的 per-instance 指标（连接数、推送成功率、广播次数、被踢次数）；
2. 把 `GatewayRouter` 的 2s 轮询换成 etcd watch（接口不变，内部替换即可）；
3. 网关实例端口分配纳入部署模板（避免与 50051-50058 冲突）；
4. 客户端按 PR-7 实现自动重连后，PR-6 的下线收益才能兑现。