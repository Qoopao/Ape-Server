# 后端待办与改进清单

> 状态：待办清单（部分已被取代）  
> 权威性：本文是「P0/P1/P2 待办」的唯一来源；已完成项以 status.md 为准  
> 最后核对：2026-11-26 @ `03ed7c2` + 工作区改造  
> 适用代码：全仓库

> 优先级以本文为准；已完成/已取代的条目在 [status.md](status.md) 标记，不再从这里删除。

> 注意：文中 `im_message_receipts`、Snowflake 单聊 seq 等是待办写作时的历史背景；现状以 [../design/messaging.md](../design/messaging.md) 和 [schema.md](schema.md) 为准。

> 本文只回答一件事：**要让这套后端既能上线、又经得起面试追问，应该改什么、怎么改、改完怎么验收。**
> 不改需求，[requirements.md](../requirements.md) 仍是需求基线；本文是"动手清单"。
> 编写日期：2026-11-20

### 与其他文档的关系（2026-11-21 对齐）

[gateway.md](../design/gateway.md) 第 9、10 章确定了登录同步的新模型（按会话取有界快照 + 每会话水位），因此本文以下三条需要调整优先级：

| 条目 | 处理 |
|---|---|
| **P0-3** 离线补投全量扫描 | **已被取代**：改为「按会话同步」（[gateway.md](../design/gateway.md) §10），不再做全局水位与定期全量对账 |
| **P0-4** receipts 逐条 → 连续水位 + 稀疏洞 | **降级为 P2/可选**：按会话同步后读放大已从 O(全部历史) 降到 O(50×20)，SACK 方案留到「单会话积压极大」时再评估 |
| **P0-5** ACK 批量化 | **保留，降为 P1**：ACK 不再阻塞同步流程，只影响 RPC 次数 |

其余 P0 条目（协议拆分、未知 type、撤回、密码 KDF、限流、踢旧连接、`SendMessages` deadline）**仍然有效**；多实例相关改造见[gateway.md](../design/gateway.md) PR-1～PR-8。

## 分级说明

| 级别 | 判定标准 |
|---|---|
| **P0** | 面试官追问两轮就会穿，或上线即出事故。必须改。 |
| **P1** | 单机能跑、多机必挂，或规模上去后性能塌方。扩容前必须改。 |
| **P2** | 技术债，不影响正确性，但影响"这项目是不是认真做的"的判断。 |

---

## P0

## P0-1 拆分 `type=101` 的双重语义

**现状**：`src/gateway/ws_session.cpp` 里，`doRead()`（认证前）把 `101` 分派给 `handleAuth`（登录，`data` 是明文密码字节），`doReadLoop()`（认证后）把 `101` 分派给发消息分支（`data` 是 `sdkws.SendMessageReq`）。两者的响应 `type` 也都是 `101`。

**为什么要改**：这是全项目最容易被一句话问死的地方。面试官不需要懂你的代码，只要问一句"登录和发消息用一个消息号，你怎么区分"，你就得解释"看连接状态"——而连接状态是服务端内存里的隐式上下文，不是协议的一部分。协议不能自描述，就是设计缺陷。

**改法**：
1. `proto/sdkws.proto` 新增 `SEND_MSG = 1` 之类的登录语义码；首帧只接受 `1`（登录）与 `100`（注册）。
2. 认证后的 `101` 只表示发消息。
3. 一个版本内兼容旧客户端：网关在首帧同时接受 `1` 和 `101`；认证后收到 `101` 才处理为发消息。
4. 响应 `type` 必须与请求 `type` 严格相等，不允许"登录成功也回 101"。

**验收**：`grep -n "case 101\|reqType == 101"` 在网关只剩一处；抓包可见登录请求与发送请求 type 不同。

---

## P0-2 未知 type 不能是黑洞

**现状**：认证后收到未实现的 type（`102/103/105` 及其他），`doReadLoop()` 只 `spdlog::debug` 然后 `continue` —— 不回帧、不断开。客户端 `RemoteFriendDao`/`ChatService` 会一直挂在 pending 里等超时。

**为什么要改**："服务端不响应"是分布式系统里最坏的失败模式，比报错更糟——调用方无法区分"慢"和"不支持"。面试里这是可靠性的基本盘。

**改法**：回 `SdkWSResp{type:0, errorCode:"INVALID_PARAM", errorMsg:"unsupported message type: N", requestId: 原值}`；同一连接累计 3 次未知 type 主动断开（防协议扫描/脏客户端）。

**验收**：发一个 `type=999` 帧，1 秒内收到错误帧而非超时。

---

## P0-3 离线补投从"全量扫描"改为"水位 + 批量去重"（**已被取代**：见文首说明与[gateway.md](../design/gateway.md) §10）

**现状**：
- `scanOfflineMessages()` 的游标 `after` 恒从 **0** 开始，每次登录全量扫 Mongo。
- 每条候选消息都要过 `WithFriendMessage(..., requireUndelivered=true)`，内部是一次 `im_message_receipts` 查询 —— 即 **N 条消息 = N 次 MySQL 往返**。
- 每批 50 条下发后 `waitOfflineBatch()` 等 ACK，超时 60s。

**为什么要改**：这是"规模一上去就塌"的典型。一个 3 个月没登录、积压 20 万条的用户，登录时是 4000 批 × (50 次 MySQL + 最多 60s 等待)。最坏情况登录要挂几小时，且期间该连接被补投占满。

**改法（两层，性能与正确性分离）**：

1. **性能层：信客户端水位。** `SdkWSReq.cursors` 已存在，登录首帧带每会话本地最大 seq；扫描起点可用 `max(0, cursor - 安全窗口)`。
   - 安全窗口（如 200 条）用来容忍客户端水位不可信/回退。
2. **正确性层：仍然只信 receipts。** 客户端的 `cursors` 只用于**减少扫描量**，绝不用来判定"已投递"。逐条去重仍然以 `im_message_receipts` 为准。
3. **把 N 次查询压成 1 次**：每批改成「先 Mongo 扫出 50 条候选（只取 `serverMsgID` 列表）→ 一条 `SELECT server_msg_id FROM im_message_receipts WHERE owner_account=? AND server_msg_id IN (...)` → 在内存里差集」。50 条候选从 50 次往返降到 1 次。
4. **Mongo 查询去掉 `status != 2` 过滤**：`status=2` 是"已投递"标记，ACK 越积越多后该条件的选择性越来越差，且它和 receipts 表达的是同一件事（重复的真相来源）。扫描只按 `{recvID, seq}` 索引区间取，去重交给 receipts 批量查询。

**验收**：构造 20 万条积压 + 已确认 19.9 万条，登录补投应在 30 秒内完成（当前应为小时级）；日志中 MySQL 查询次数 ≈ Mongo 批次数的 2 倍以内，而非 50 倍。

---

## P0-4 `im_message_receipts` 从"逐条"升级为"连续水位 + 稀疏洞"（**降级为 P2/可选**：见文首说明）

**现状**：`im_message_receipts(owner_account, server_msg_id, conversation_id, seq, delivered_at)`，`PRIMARY KEY(owner_account, server_msg_id)`。每条被确认的消息一行。

**先说清楚：逐条记录是**对**的。** 代码注释写得很明确——不能用"最大 ACK seq"当水位，因为 ACK 会乱序（客户端可能先确认 104 再确认 103），水位一旦推到 104 就把 103 永久跳过了。这个判断是这套设计里最值得讲的一处。

**问题在存储与读放大**：1 亿条消息 × 2 人 = 2 亿行；用户每次登录补投都要在这张表上做区间查询。

**改法：TCP SACK 思路 —— 连续水位 + 稀疏洞集合。**

```
im_conv_delivery(
  owner_account BIGINT, conversation_id VARCHAR(64),
  acked_contiguous_seq BIGINT,   -- 从 min_seq 起连续已 ACK 的最高 seq
  PRIMARY KEY(owner_account, conversation_id)
)
im_delivery_holes(               -- 水位之上、已 ACK 但未连续的点
  owner_account BIGINT, conversation_id VARCHAR(64), seq BIGINT,
  PRIMARY KEY(owner_account, conversation_id, seq)
)
```

- 收到 ACK(seq)：若 `seq == acked_contiguous_seq + 1`，水位推进，并 `SELECT ... WHERE seq > 新水位 ORDER BY seq` 逐个检查洞集合能否连续合并（合并即删洞并继续推进）；否则插入洞集合。
- 判定"是否已投递"：`seq <= acked_contiguous_seq` 走主键单点读（绝大多数情况）；否则查洞集合（极少）。
- 保留一条"空洞异常"监控指标：洞数量持续增长 = 客户端 ACK 路径有问题。

**代价与取舍（面试要主动说）**：这是典型的用写入复杂度换读取复杂度。ACK 是高频写路径，多了一次"检查洞并合并"的读；收益是登录补投的读放大从 O(消息数) 降到 O(会话数 + 洞数)。如果你的场景是"读多写少"（补投频繁、单用户消息量大），这个交换成立。

**验收**：1 亿消息量级下，登录补投的 receipts 相关查询次数 ≈ 会话数；`im_delivery_holes` 行数应长期接近于 0。

---

## P0-5 ACK 批量化（**保留，降为 P1**：见文首说明）

**现状**：`handleAck()` 对 `AckReq.servermsgids` 逐个 `co_await push_client_->AckMsg(...)`，N 条 ACK = N 次 gRPC（每次 8s deadline、失败重试 2 次间隔 200ms）。50 条一补投批次 = 50 次同步 RPC 卡在读循环里。

**为什么要改**：这是"批处理思维"的送分题。面试官问"你的 ACK 怎么设计的"，答"一条一个 RPC"会被反问"客户端一次确认 50 条你怎么办"。

**改法**：
1. `proto/push.proto` 加 `AckMsgBatchReq{userID, repeated AckMsgItem{serverMsgID, seq}, ackType}` / `AckMsgBatch`（保留旧 `AckMsg` 一个版本）。
2. 服务端一次事务批量 `INSERT ... ON DUPLICATE KEY UPDATE` + 批量排 outbox。
3. 网关侧攒批：10ms 窗口内到达的 ACK 帧合并成一个 RPC（客户端本来就是整批发 `serverMsgIDs`，实际只需改"逐条调用"为"一次调用"）。

**验收**：补投 50 条并 ACK，`grep` PushService 日志应为 1 次而非 50 次。

---

## P0-6 撤回能力必须落地

**现状**：旧 `RevokeMsg` RPC 已从 `msg.proto` 移除（本次清理未参与主链路的 stub）；若要实现撤回，需要重新增加协议。`MsgData.isRecalled` 等字段仍在 `sdkws.proto`，但当前没有链路使用。

**为什么要改**：这是 IM 的"标配功能"。面试官问"支持撤回吗"，答"协议留了字段但没实现"，等于承认项目完成度不足；而如果能把撤回的设计讲清楚（时限、权限、幂等、双端一致性、群聊扩散），反而能加分——但前提是你**真做出来了**。

**改法**：
1. `RevokeMsg(conversationID, seq, userID)` 实现：校验发送者本人（或群主/管理员）、时限（建议 2 分钟，可配置）、消息存在且未撤回。
2. Mongo `update_one({serverMsgID}, {$set:{isRecalled:true, content:""}})` —— **正文要真删**，只留元信息，否则"撤回"名不副实。
3. Kafka / 会话内通知 `RevokeMsgTips{revokerUserID, seq, conversationID, revokeTime, isAdminRevoke}`。
4. 更新 `im_conversations.latest_message` 预览为「消息已撤回」。
5. **幂等**：重复撤回返回成功，不重复发通知。

**验收**：A 撤回后 B 客户端 1 秒内看到占位；Mongo 中该文档 `content` 为空；重复调用无第二条通知。

---

## P0-7 密码哈希必须换成 KDF

**现状**：`SHA-256(salt ‖ password)` 单轮，盐是 UUID 字符串前 32 字符（含 4 个连字符，实际熵 < 128 bit），比较用 `!=` 普通字符串比较。

**为什么要改**：这是安全审计第一条就会被打回的设计。单轮 SHA-256 在 GPU 上是每秒百亿次量级，等于明文存密码。

**改法**：
1. 换 Argon2id（或 bcrypt cost≥12 / PBKDF2-HMAC-SHA256 迭代≥60 万）。
2. 盐用 `std::random_device` 或 CSPRNG 取 16–32 字节。
3. 比较用 `CRYPTO_memcmp`（恒定时间），OpenSSL 已在依赖里。
4. 平滑迁移：`users.password_algo` 列标记 `sha256_v1` / `argon2id_v2`，登录成功时若为旧算法则用明文重算并原地升级（rehash on login）。

**验收**：并发 1000 QPS 登录下 hash 耗时可控（Argon2id 需调参数到 ~50ms）；DB 中不再出现 64 位 hex。

---

## P0-8 连接级与用户级限流

**现状**：全项目无任何令牌桶/QPS 限制。唯一"流控"是 Kafka lane 的 128/64 背压和 WS 写队列 256。

**为什么要改**：一条连接可以无限刷消息、无限发好友申请、无限试密码。前面所有可靠性设计在"被刷"面前失效。

**改法**：
1. 进程内令牌桶（或 Redis `INCR + EXPIRE` 滑动窗口）三档：
   - 发送消息：20 msg/s/用户（超限回 `RATE_LIMITED`，不回断连）
   - 好友申请：10/min/用户
   - 登录失败：5/min/账号 + 20/min/IP（失败计数落 Redis，成功清零）
2. 单连接并发在途请求上限（如 32），超出直接回错误帧。

**验收**：脚本 1000 msg/s 压单连接，服务端稳定、其余连接延迟不受影响；错误码为 `RATE_LIMITED` 而非静默丢弃。

---

## P0-9 同账号重复登录必须踢掉旧连接

**现状**：`WSSessionManager::registerSession` 直接 `map[account] = newSession` 覆盖旧指针，**旧 session 既不被关闭也不感知自己被顶替**；它的 `onDisconnect` 因 `it->second != self` 判为 stale，连在线状态都不回写。结果：僵尸连接占着 strand 和 socket，客户端以为自己还在线。

**改法**：`registerSession` 时若已存在旧 session：
1. 先给旧连接发一帧 `type=0, errorCode:"KICKED", errorMsg:"account logged in elsewhere"`；
2. `co_spawn` 调用旧 session 的关闭流程（置 `disconnected_` + 关 socket）；
3. 再登记新 session。
4. 明确产品语义：是"互斥登录"还是"多端在线"。当前是互斥，就要把互斥做干净。

**验收**：同一账号两端登录，旧端 1 秒内收到 KICKED 帧并断开；`user:{acc}:online` 仍为 `"1"`（新连接在线）。

---

## P0-10 `SendMessages` 补 deadline 与条数上限

**现状**：`MsgClient::SendMessages` 是唯一**没设 gRPC deadline** 的调用（其余 8–15s）。且 `msgs_size()` 只校验 >0，无上限。

**改法**：deadline 10s；`msgs_size() > 20` 回 `INVALID_ARGUMENT`；单条 `content` 上限保持 64 KiB。

**验收**：人为让 MsgService 卡住，网关应在 10s 内回 `500` 而非无限挂起。

---

## P1

## P1-1 多实例下的 Session 路由

**现状**：`WSSessionManager` 是进程内单例；`PushService::getGatewayPushClient()` 硬编码 `localhost:50055`。网关一扩到 2 个实例，一半推送必然 `success=false`。

**改法**：
1. Redis 记录 `gw:user:{account} → {instanceId, ts}`，TTL 与心跳同步（如 60s，心跳续期）。
2. PushService 推之前先查路由表，再按 `instanceId` 查 etcd 拿到地址，调对应实例的 `GatewayPushService`。
3. 网关实例定期全量对齐（Redis set 与本进程 session 表做差集，清理残留键）。
4. 跨实例推送失败 → 落离线（当前行为正确，保留）。

**验收**：起 2 个网关，A 用户连 gw1、B 连 gw2，A 发消息给 B，B 能即时收到；杀掉 gw2 后 Redis 键在 TTL 内失效。

---

## P1-2 方法级服务发现替代硬编码地址

**现状**：`main.cpp` 给每个服务注册了 `methods` 列表，`BackbonService` 把它写进 Redis（TTL 60s 且**无刷新**）。但网关 `discover_Service` 只取 `ipport(0)` 建 channel，路由是硬编码 if/else；`methods` 从没参与过路由。

**改法**：
1. `methods` 的 Redis TTL 提到租约同级（300s）并由 KeepAlive 刷新，或干脆合并进 etcd value 的 JSON。
2. 网关启动时拉全量 `(service, method) → addr`，订阅变更（Redis pub/sub 或 ectd watch）。
3. 消息分发改为查表：`type → (service, method) → channel`，`type` 映射表外置成配置。

**验收**：新增一个 RPC 无需改网关代码，只改注册清单 + 配置表。

---

## P1-3 `min_seq` 与"清空会话"语义

**现状**：`im_conversations.min_seq` **从无写入点**（恒 0）；`ClearUserConversationMsg`/`SetConversationMinSeq` 全未实现；会话列表的未读插入条件 `min_seq < seq` 恒真。

**改法**：
1. 实现 `ClearUserConversationMsg(owner, conversationID)`：`min_seq = max_seq + 1`、`unread_count = 0`、删未读明细；Mongo 正文按保留策略异步删。
2. `GetSingleChatHistory` 的下界改为 `max(afterSeq, deleted_seq, min_seq)`。
3. 会话列表、未读计算统一用 `min_seq` 当"历史可见下界"。

**验收**：清空后历史接口返回空、未读为 0、新消息从新 seq 起正常显示。

---

## P1-4 混链拉取（会话列表首屏）

**现状**：`PullMixList`/`FetchUserMessageList`/`PullMessageBySeqs` 全未实现。客户端只能"先拉 240 会话列表（含 latest_message 摘要 JSON）+ 再逐个聊 244"，首屏要 N+1 次往返。

**改法**：实现 `FetchUserMessageList(userID, cursor, limit, forward, news)`：按 `im_conversations.latest_time` 分页取会话 + 每会话取最近 K 条（Mongo 按 `{convID, seq}` 倒序 limit K，一次批量聚合）。

**验收**：首屏 20 个会话，1 次请求拿到全部摘要 + 最近消息。

---

## P1-5 `logID` 回填与错误码枚举化（**logID 已完成，2026-11-26**）

**现状**：`SdkWSResp.logID` 已回填 trace id（101/244/245/246 + 104；见 [design/observability.md](../design/observability.md)）；错误码仍是散落字符串，`INVALID_PARAM` 一词表示"缺 account / token 无效 / 载荷解析失败"三种完全不同的处置方式。剩余工作只剩错误码枚举化。

**改法**：
1. 网关为每个请求生成 `logID`（= 当前 OTel traceId），回填进响应。
2. `proto` 定义 `enum ErrorCode`，停止新增数字码。
3. `INVALID_PARAM` 拆为 `MISSING_ACCOUNT` / `TOKEN_INVALID` / `BAD_PAYLOAD`，客户端据此决定"重新登录"还是"改参数"。

**验收**：任取一条客户端报错，能用 `logID` 在 Jaeger 里定位到完整链路。

---

## P1-6 启动失败必须返回非 0

**现状**：`main()` 用一个大 `try/catch` 包住全部初始化，异常只 `spdlog::error` 然后 `return 0`。证书路径错误 / 端口占用 / DB 连不上，进程都以成功退出。

**改法**：`return 1`；区分"可降级"（etcd 注册超时）与"不可启动"（证书、端口、DB）；后者拒绝启动。

**验收**：删掉 `server.crt` 启动，`echo $?` 为 1，容器编排能正确重启/告警。

---

## P2

## P2-1 配置外置

除 `SNOWFLAKE_WORKER_ID`/`SNOWFLAKE_DATACENTER_ID` 外全部硬编码：Redis/MySQL/Mongo/Kafka/etcd 地址、网关端口、证书绝对路径（`/root/ape-server/scripts/certs/...`）。→ 引入 `config/{env}.yaml`（nlohmann_json 已在依赖里），支持环境变量覆盖。

## P2-2 清理遗留物

| 遗留物 | 处置 |
|---|---|
| `pending_ack:online:*`、`pending_ack:offline:*`、`pending_ack:deadlines`、`msg_conv:*`、`msg:consumed:*` | 只有删除逻辑无写入 → 全部移除 |
| `PushService.AddPendingOfflineAck` | 返回 `UNIMPLEMENTED` → 从 proto 删或 `reserved` |
| `docker/mysql/init.sql` 的 `groups`/`group_members`/`conversations` | 运行时不读写 → 删表或标注 deprecated |
| `offline_msg_topic` | 已创建但代码不生产不消费 → 删除 |
| `WSSession::write_drain_running_` | 声明后从未使用 → 删 |
| `WebServer` 四个 client getter | 无调用者 → 删 |

## P2-3 分层重构

friend/group/conversation 三域的业务逻辑全在 `src/storage/mysqlhandler.cpp`（该文件已过 2000 行），三个 `server.cpp` 合计仅 262 行做 reactor 转发。→ 抽 `FriendRepository`/`GroupRepository`/`ConversationRepository`，service 层持事务编排，storage 层只留 SQL。

**为什么值得改**：面试问"你的分层怎么做的"，现在的答案是"网关 → service（转发）→ 一个 2000 行的 MySQLHandler"，撑不住。

## P2-4 索引与锁

1. `im_groups` 缺 `owner`、`name`、`dismissed` 索引。
2. `GroupService` 的 `Search` 对群行加 `FOR UPDATE`（与所有写操作串行化）→ 改无锁读。
3. `friend_pairs` 只增不删 → 加定期清理或改为 `INSERT IGNORE` + 归档。
4. `im_conversation_unread` 行数随未读增长 → 配合 P1-3 的 `min_seq` 一起治理。

---

## 附：改完之后怎么讲（面试话术对照）

| 话题 | 改之前（会被追穿） | 改之后（可以主动讲） |
|---|---|---|
| 消息可靠性 | "用 Kafka 保证不丢" | "MySQL outbox 是状态机，Kafka 只是投递通道；可靠性靠 `im_message_receipts` 的持久 ACK 回执，Kafka 挂了 outbox 会重试" |
| 消息序号 | "用 Snowflake 生成 seq" | "单聊 seq 要跨会话全局有序（方便排序/分页），用 Snowflake；群聊 seq 要**会话内连续**（客户端要检测缺口），用 `last_seq+1` 行锁分配。两种需求不同，所以两套" |
| 已投递判定 | "收到 ACK 就标记已投递" | "ACK 会乱序，所以不能用水位；我用连续水位 + 稀疏洞（SACK 思路），常态 O(1) 单点读，异常才查洞集合" |
| 离线补投 | "登录时把离线消息推下去" | "客户端上报 `cursors` 只用于**性能**（减少扫描量），正确性仍然只信 receipts——客户端水位可以被伪造，回执不能" |
| 幂等 | "用唯一键防重" | "三层：`dedupe_key = sendID:clientMsgID` 唯一键防重发；`(group,sender,client_id)` 防群消息重放；`im_group_requests` 存 hex(payload)+reply 做到**重放返回上次结果**而不只是拒绝" |
| 并发控制 | "用事务" | "好友关系用 `friend_pairs` 主键行当互斥锁（`INSERT ... ON DUPLICATE KEY UPDATE`），把关系变更与消息提交串行化，READ COMMITTED 下就不会有 A 删好友的同时 B 在发消息的竞态" |
| 限流 | "还没做" | "三档：连接级在途上限、用户级令牌桶、账号/IP 级登录失败计数；限流拒绝回业务错误码不断连，避免重连风暴" |

---

## 落地顺序建议

如果只有 3 天：
1. P0-1、P0-2、P0-10（协议与超时，半天）
2. P0-9、P0-5（踢旧连接、批量 ACK，半天）
3. P0-7、P0-8（安全与限流，一天）
4. P0-6（撤回，一天）

如果有一周，追加 P0-3 + P0-4（补投与 receipts，这是整套设计里最有讲头的一块），再补 P1-5（logID + 错误码枚举，排障与面试都好讲）。