# 群聊投递架构改造方案 v2（已废弃）

> 状态：历史（已废弃）  
> 权威性：本文是旧群聊投递方案，已被 v3 取代  
> 最后核对：2026-11-26 @ `03ed7c2` + 工作区改造  
> 适用代码：src/services/group_service/

> 已被 [../design/group-chat.md](../design/group-chat.md) 取代，**不得作为现状**。

> 状态：待实施  
> 适用代码：`/root/ape-server`（当前群聊服务）  
> 关联文档：`docs/多网关改造方案.md`、`docs/多网关改造实施记录.md`  
> 目标：把群聊投递从「所有 GroupService 实例每 2 秒轮询、逐成员 fan-out」改为「Kafka 事件驱动 + 群级投递水位 + 小群推送/大群 notify+pull」，支持 GroupService 水平扩展。

---

## 1. 背景

当前群聊投递的核心逻辑在 `src/services/group_service/server.cpp` 的 `Deliver()`：

- 每 2 秒执行一次 `QueryGroupsWithPending`；
- 每个 GroupService 实例都跑同一个循环；
- 对每个群读取 `GroupActiveMembers`，逐成员构造 notice，再走 `PushBatch(251)`；
- 没有「这个群投递到哪了」的持久化状态，只能靠成员 `ack_seq` 反推。

问题：

1. **多实例重复投递**：没有归属，所有实例都会处理同一个群。
2. **重复 fan-out**：N 条消息产生 N 次投递循环，每次都要读成员、发推送。
3. **大群成本不可控**：万人群也逐成员推送。
4. **延迟固定 0–2 秒**：靠轮询，且 DB 扫描随群数量增长。
5. **无法回答「群投到哪」**：缺少 `delivered_seq`，运维无法判断积压。
6. **网关 GroupClient 单地址**：GroupService 多实例时命令仍只打第一台（前置问题，见 18.3）。

---

## 2. 目标与非目标

### 目标

- 群消息写入成功即可靠，不依赖 Kafka 成功；
- 同一群同一时刻只有一个投递 owner；
- 群级投递水位 `delivered_seq`，N 条消息合并成一次投递；
- 小群推送消息内容，大群只推通知，客户端按 `ack_seq` 拉取；
- GroupService 可以水平扩展；
- Kafka 故障时 DB 扫描兜底，不丢消息；
- 保持每群 seq 稠密、严格 +1；
- 保留每成员 `ack_seq/read_seq`，支持未读和已读回执。

### 非目标

- 不改单聊 seq/ACK；
- 不做多设备同步（当前按单设备设计）；
- 第一阶段不做网关本地 fan-out（Phase 3 再做）；
- 不做跨群全局顺序（群之间无序）；
- 不重写历史群消息表。

---

## 3. 术语

| 术语 | 含义 |
|---|---|
| `last_seq` | 群当前已分配的最大 seq，稠密、严格 +1 |
| `delivered_seq` | 群已发起过投递的最大 seq；不表示成员已读 |
| `ack_seq` | 某成员已处理到的群 seq，用于离线拉取/未读 |
| `read_seq` | 某成员已读到的群 seq，用于已读回执 |
| `target` | 本轮投递快照目标 seq，通知和 `delivered_seq` 都用它 |
| owner | 当前负责投递某个群的实例 |
| 小群 | 成员数 ≤ `GROUP_PUSH_MAX_MEMBERS` |
| 大群 | 成员数 > `GROUP_PUSH_MAX_MEMBERS` |

---

## 4. 设计决策与取舍

| 编号 | 决策 | 取舍 |
|---|---|---|
| D1 | **不使用 per-message 群 outbox**：写入提交后 best-effort 发 Kafka，失败靠 DB 扫描兜底 | 少一张 outbox、少 N 行任务锁；代价是事件可能延迟 1–2 秒，靠扫描补 |
| D2 | **`delivered_seq` 独立成表** | 避免投递更新和 `im_groups.last_seq` 抢同一把行锁；代价是多一张表 |
| D3 | **Kafka partition by groupID + MySQL 租约** | Kafka 保证同群有序、分摊负载；MySQL 租约保证同一群同一时刻只有一个 owner；代价是租约过期可能重复投递 |
| D4 | **小群推消息内容，大群 notify+pull** | 小群实时性好；大群 fan-out 可控；代价是大群客户端多一次拉取 |
| D5 | **群 seq 保持稠密** | 客户端可用 `+1`/区间；代价是同群发送串行（`last_seq` 行锁） |
| D6 | **at-least-once** | 实现简单、Kafka/扫描都能重试；客户端按 `seq` 去重、按 `seq` 排序 |
| D7 | **每成员保留 `ack_seq/read_seq`** | 支持离线拉取/未读/已读；`delivered_seq` 不承担逐成员状态 |
| D8 | **Phase 1 不做网关 fan-out** | 先扛千人大群；万人大群 Phase 3 再上网关本地 fan-out |
| D9 | **过期 ACK no-op、越界 ACK 拒绝、返回生效水位** | 与单聊 P0-2 对齐；避免过时确认断连/写脏；代价是客户端要处理 `INVALID_SEQ`/`cursor` |

### 4.1 为什么不用群 outbox

如果每条群消息插一行 outbox：

- 行数 = 消息数；
- worker 逐条处理时更新同一个群的 `delivered_seq`，等于每条消息一次行锁；
- 投递本来就可以合并成「一个区间一次通知」，per-message 任务没有收益。

因此群聊采用 **事件 + 水位扫描**，不采用单聊那种 per-message outbox。

### 4.2 为什么 `delivered_seq` 不放在 `im_groups`

`im_groups.last_seq` 是发送热点。每个发送事务都要 `UPDATE im_groups SET last_seq=...`。

如果 `delivered_seq` 也在同一行：

- 投递 worker 每次推进水位都要锁同一行；
- 和发送事务互相等待；
- 热群吞吐直接下降。

独立表后：

- 发送只锁 `im_groups`；
- 投递只锁 `im_group_delivery`；
- 两者互不阻塞。

### 4.3 为什么同群发送仍然串行

稠密 seq 要求：

```sql
UPDATE im_groups SET last_seq = last_seq + 1 WHERE id = ?;
```

这是同一群的行锁，无法避免。它是毫秒级单行更新，不是 outbox 放大。

如果未来要降低同群发送串行度，只能改 seq 模型：

| 方案 | 代价 |
|---|---|
| 批量预分配 seq | 失败留空洞，客户端要容忍非稠密 |
| Redis INCR | 持久化/故障恢复复杂，可能重复或丢号 |
| 按发送者分 seq | 群内没有全局顺序 |
| 拆子群 | 业务语义变化 |

**本方案不采用**，先接受同群提交串行，优先去掉「每消息 outbox + 每消息投递」的放大。

---

## 5. 目标架构

```
发送群消息
  │
  ▼
GroupService（任意实例）
  └─ MySQL 事务：
       ├─ INSERT im_group_messages(seq)
       ├─ UPDATE im_groups SET last_seq=seq, updated_at=now()
       └─ 提交
  │
  ├─ 提交后 best-effort produce Kafka: group-events, key=groupID
  │
  ▼
Kafka group-events（32 partitions，key=groupID）
  │
  ▼
GroupService 消费者组 group-delivery（按 partition 归属）
  │
  ▼
DeliverGroup(groupID)
  ├─ 抢 im_group_delivery 租约（同一群只有一个 owner）
  ├─ target = 当前 last_seq（快照）
  ├─ 小群：读 (delivered, target] 消息，Notice{messages} 推在线成员
  ├─ 大群：只发 Notice{lastSeq=target}，客户端 pull
  └─ UPDATE delivered_seq = target，释放租约
  │
  └─ DB 扫描兜底：每 2s 找 last_seq > delivered_seq 的群，重复上述流程
```

---

## 6. 数据模型

### 6.1 `im_group_delivery`（新增）

```sql
CREATE TABLE IF NOT EXISTS im_group_delivery (
  group_id       BIGINT UNSIGNED NOT NULL PRIMARY KEY,
  delivered_seq  BIGINT UNSIGNED NOT NULL DEFAULT 0,
  last_notify_at BIGINT NOT NULL DEFAULT 0,
  lease_owner    VARCHAR(64) NOT NULL DEFAULT '',
  lease_until    BIGINT NOT NULL DEFAULT 0,
  updated_at     BIGINT NOT NULL DEFAULT 0
) ENGINE=InnoDB DEFAULT CHARSET=ascii;
```

初始化/回填：

```sql
-- 已有群的历史消息视为已投递，避免上线瞬间全量重推；
-- 上线后新产生的消息从 last_seq+1 开始进入待投递状态。
INSERT IGNORE INTO im_group_delivery
  (group_id, delivered_seq, last_notify_at, lease_owner, lease_until, updated_at)
SELECT id, last_seq, 0, '', 0, 0 FROM im_groups;
```

### 6.2 `im_groups` 增加 `updated_at`（用于扫描）

```sql
ALTER TABLE im_groups
  ADD COLUMN updated_at BIGINT NOT NULL DEFAULT 0,
  ADD KEY group_updated (updated_at);
```

发送事务中同步更新：

```sql
UPDATE im_groups
SET last_seq = ?, updated_at = ?
WHERE id = ?;
```

> 注意：这是同一行的另一个列，不增加新的行锁；发送本来就要更新 `last_seq`。  
> 迁移需在代码中先查 `information_schema.columns`，缺列才执行 `ALTER`，避免重复启动报错。

### 6.3 沿用现有表

```sql
im_group_messages (
  group_id, seq, id, client_id, sender, nickname, text, created_time,
  PRIMARY KEY(group_id, seq)
);

im_group_members (
  group_id, account, active, epoch, join_seq, ack_seq, read_seq,
  PRIMARY KEY(group_id, account)
);
```

### 6.4 明确不新增

- **不新增** `im_group_outbox`；
- **不新增** 逐成员投递状态表；
- 逐成员状态只保留 `ack_seq/read_seq`。

---

## 7. 写入路径

### 7.1 事务

`MySQLHandler::ExecuteGroupCommand` 的 `Send` 分支：

1. 校验成员、群状态、`epoch`；
2. `seq = last_seq + 1`（同一事务内 `SELECT ... FOR UPDATE` 或 `UPDATE` 返回）；
3. `INSERT INTO im_group_messages`；
4. `UPDATE im_groups SET last_seq=seq, updated_at=now()`；
5. 提交。

### 7.2 Kafka 事件

提交成功后：

```cpp
GroupDeliveryEvent event;
event.set_groupid(groupID);
event.set_seq(seq);
producer->produce("group-events", groupID, event.SerializeAsString());
```

- **best-effort**：失败只记日志，不回滚消息；
- 不等待 broker 确认（不需要 `deliverConfirmed`）；
- 事件丢失由 DB 扫描兜底。

### 7.3 新增 proto

`proto/groupchat.proto` 增加：

```proto
message GroupDeliveryEvent {
  string groupID = 1;
  uint64 seq = 2;
}
```

> 事件只用于「尽快触发」，消费端以 DB 的 `last_seq` 为准，`seq` 仅用于日志/去重。

### 7.4 幂等

- 同一 `(group_id, sender, client_id)` 已存在：返回原消息，不重复分配 seq；
- 沿用现有 `sender_request` 唯一键。

---

## 8. 投递路径

### 8.1 owner 与租约

Kafka 负责：

- 同群消息进同一 partition；
- 同一 partition 同时只有一个消费者；
- 保证同群事件处理有序。

MySQL 租约负责：

- 事件消费者和 DB 扫描可能同时尝试同一个群；
- 只有抢到租约的实例才能投递。

抢租约：

```sql
UPDATE im_group_delivery
SET lease_owner = ?, lease_until = ?, updated_at = ?
WHERE group_id = ?
  AND delivered_seq < ?          -- 还有待投递消息
  AND (lease_until < ? OR lease_owner = ?);
```

- `affected_rows == 1`：抢到；
- 否则跳过，稍后由 owner/扫描处理；
- 租约默认 30 秒，长批次可续租。

### 8.2 投递伪代码

```cpp
awaitable<void> DeliverGroup(string groupID) {
  if (!await ClaimGroupLease(groupID, owner, 30s))
    return;

  uint64 last      = await GetGroupLastSeq(groupID);        // 快照
  auto delivery    = await GetGroupDelivery(groupID);
  uint64 delivered = delivery.delivered_seq;
  if (last <= delivered) { await ReleaseLease(groupID); return; }

  GroupInfo group   = await GetGroup(groupID);
  uint64 target     = last;                                 // 大群：一次到最新
  vector<Message> messages;
  std::vector<uint64_t> members = await GroupActiveMembers(groupID);
  std::vector<uint64_t> online  = await FilterOnlineMembers(members);

  bool silent       = false;   // 静默：推进水位但不发通知（客户端自行 pull）
  bool defer        = false;   // 延迟：不推进，下一轮再通知
  if (group.member_count <= GROUP_PUSH_MAX_MEMBERS) {
    // 小群：限制单批消息数，避免一次读太多
    target   = min(last, delivered + GROUP_MESSAGE_BATCH);
    messages = await GetGroupMessages(groupID, delivered, target);
  } else {
    if (online.size() > GROUP_NOTIFY_MAX_ONLINE) {
      silent = true;           // 超大在线规模：只推进，不推送
    } else if (now() - delivery.last_notify_at < GROUP_NOTIFY_INTERVAL_MS) {
      defer = true;            // 合并窗口内：不推进，等窗口结束再通知
    }
  }

  bool deliveredOk = true;
  if (!silent && !defer) {
    PushResult result = (group.member_count <= GROUP_PUSH_MAX_MEMBERS)
      ? await PushNotice(groupID, target, messages, members)      // 携带正文
      : await PushNotice(groupID, target, {}, online);           // 只带 lastSeq
    // Unknown 表示有网关未确认；不推进，等扫描/事件重试
    deliveredOk = (result != PushResult::Unknown);
  }

  if (deliveredOk && !defer) {
    // 必须用本轮快照 target，不能用更新后的 last_seq
    await AdvanceGroupDelivered(groupID, target, owner);
    if (!silent)
      await MarkGroupNotified(groupID, now());
  }
  await ReleaseLease(groupID);

  // defer：不推进，扫描会在窗口结束后重新处理；
  // target < last：下一轮继续处理剩余消息。
}
```

### 8.3 推进水位（CAS）

```sql
UPDATE im_group_delivery
SET delivered_seq = GREATEST(delivered_seq, ?),
    lease_owner = '',
    lease_until = 0,
    updated_at = ?
WHERE group_id = ?
  AND delivered_seq < ?;
```

- `target` 必须是 8.2 的快照；
- 不能用「更新时的最新 `last_seq`」，否则会跳过投递期间新到的消息；
- `GREATEST` + `WHERE delivered_seq < target` 保证只增不减。

### 8.4 循环处理

如果 `target < last_seq`（小群批次上限，或投递期间又有新消息）：

- 释放租约，让下一轮事件/扫描处理；
- 或在本轮续租并继续，但**必须重新快照 target**；
- 建议每轮最多处理 `GROUP_MAX_BATCHES`（默认 5）批，避免长期占用租约。

### 8.5 扫描兜底

每 `GROUP_SCAN_INTERVAL_MS`（默认 2000ms）：

```sql
SELECT g.id
FROM im_groups g
LEFT JOIN im_group_delivery d ON d.group_id = g.id
WHERE g.updated_at > ?
  AND g.last_seq > COALESCE(d.delivered_seq, 0)
  AND (d.lease_until IS NULL OR d.lease_until < ?)
LIMIT ?;
```

- 只扫最近 `GROUP_SCAN_WINDOW_SECONDS`（默认 3600）内活跃的群；
- 对每个群尝试 `ClaimGroupLease`，抢到才投递；
- 所有实例都可以跑扫描，租约保证互斥；
- 群数量很大时，可把 pending 群放入 Redis 集合，扫描只读集合（优化项，非必需）。

---

## 9. Kafka 设计

| 项 | 值 |
|---|---|
| topic | `group-events` |
| key | `groupID` |
| partitions | 32（生产建议 ≥ 3×GroupService 实例数） |
| consumer group | `group-delivery` |
| offset commit | 处理成功后手动 commit（沿用现有 `KafkaConsumer`） |
| 失败 | 不 commit，同一 offset 每秒重试；进程崩溃后从已提交 offset 重放 |
| 重复 | 允许；客户端按 `seq` 去重 |

### Rebalance

- 分区被 revoke：停止该分区新任务，等待当前 `PushNotice` 完成；
- commit offset 后释放；
- 新 owner 从 `delivered_seq` 继续；
- 可能重复推送，客户端按 `seq` 去重。

### 为什么还需要扫描

- Kafka 事件可能丢；
- 消费者可能滞后；
- rebalance 期间暂停；
- 事件 produce 失败没有重试。

扫描保证最终一致，代价是最坏延迟约 `GROUP_SCAN_INTERVAL_MS`。

---

## 10. 小群 / 大群 / 超大群

阈值：

- `GROUP_PUSH_MAX_MEMBERS`：默认 500；
- `GROUP_NOTIFY_MAX_ONLINE`：默认 5000；
- `GROUP_MESSAGE_BATCH`：默认 50；
- `GROUP_NOTIFY_INTERVAL_MS`：默认 1000。

| 规模 | 条件 | 投递方式 |
|---|---|---|
| 小群 | 成员数 ≤ `GROUP_PUSH_MAX_MEMBERS` | `Notice{groupID, lastSeq=target, messages=[...]}` 推给**所有成员**；离线成员 PushBatch 返回 NotOnline，不重试 |
| 大群 | 成员数 > 阈值，且在线成员 ≤ `GROUP_NOTIFY_MAX_ONLINE` | `Notice{groupID, lastSeq=target}` 只推**在线成员**；客户端 pull |
| 超大群 | 在线成员 > `GROUP_NOTIFY_MAX_ONLINE` | **不推送**，只推进 `delivered_seq`；客户端前台打开群/轮询时 pull |

说明：

- 小群 Notice 携带消息正文，避免客户端立刻再 pull；
- 大群只带 `lastSeq`，减少 fan-out 体积；
- 大群通知按 `GROUP_NOTIFY_INTERVAL_MS` 合并：同一群在这个窗口内最多推一次；
- 判断在线：`PushBatch` 内部按路由 MGET；大群先限制在线人数，避免无界 fan-out。

---

## 11. 群消息拉取

沿用现有 `groupchat.Command{Pull}`：

```proto
Command {
  Operation operation = Pull;
  string groupID = 3;
  uint64 after = 8;     // 客户端 ack_seq
  uint32 limit = 9;     // 服务端 clamp 到 100
  uint64 snapshot = 10; // 可选
}
```

返回 `Reply.messages`、`hasMore`、`snapshot`、`cursor`。

规则：

- 只返回 `seq > max(after, join_seq)` 的消息；
- 群必须存在且未解散；成员必须 `active=1`；
- 按 `seq` 升序；
- `Reply.cursor` 返回本页最后一个 seq；
- ACK 提前会让 `after` 跳过未处理消息；客户端必须只 ACK 连续水位；
- 热点大群可加 Redis 缓存（Phase 2 优化）。

---

## 12. ACK、已读、未读、入群

### ACK

请求：`Command{Operation=Ack, groupID, after=ackSeq}`（使用 `after`，不是 `snapshot`）。

服务端规则：

1. group 必须存在且未解散；成员必须 `active=1`；
2. `after > last_seq`：拒绝，`errorCode=INVALID_SEQ`，不写 `ack_seq`；
3. `seq = max(after, join_seq)`：入群前的消息不能被确认；
4. `ack_seq = GREATEST(ack_seq, seq)`：低水位迟到不回退；
5. 重复 ACK 幂等；
6. `Reply.cursor = 实际生效的 ack_seq`，客户端能知道是否被拒绝/截断。

```sql
UPDATE im_group_members
SET ack_seq = GREATEST(ack_seq, ?)
WHERE group_id = ? AND account = ? AND active = 1;
```

**过期/无效 ACK（与单聊 P0-2 对齐）：**

- group 不存在/已解散 → `errorCode=GROUP_NOT_FOUND`，不写、不断连；
- 成员不存在/`active=0`（已退群/被踢）→ **no-op 成功**，不写、不断连；
- `after <= 0` → 视为无效，no-op；
- 组命令走 `type=250` 的 `Reply`，错误不会断 WebSocket；网关只把 `errorCode` 返回客户端。

> 现状实现与本节差异：`executeOnPool` 的 Ack/Read 更新没有 `active=1`，成员不存在时 UPDATE 0 行仍返回成功。
> 实施时必须补 `active=1`、成员/群校验和 no-op 语义。

### 已读

沿用 `Command{Operation=Read}`，规则与 ACK 相同，只更新 `read_seq`：

```sql
UPDATE im_group_members
SET read_seq = GREATEST(read_seq, ?)
WHERE group_id = ? AND account = ? AND active = 1;
```

过期/无效 Read 同样 no-op 成功，不写、不断连。

### ACK 乱序与提前

- **低水位迟到**（先 ACK 100，后 ACK 80）：`GREATEST` 保持 100，不回退；
- **高水位提前**（`after <= last_seq`，但客户端还没处理到）：服务端无法识别，会降低未读数，
  并让后续 `Pull{after=ack_seq}` 跳过未处理消息；**客户端只能 ACK 连续已处理水位**；
- **ACK 不参与 `delivered_seq`**：乱序/提前 ACK 不影响群投递 fan-out，只影响未读、已读、Pull 起点；
- **旧 `MIN(ack_seq)` pending 判断**会被最慢成员拖住；新方案用 `delivered_seq`，不再依赖 `ack_seq` 判断 pending；
- `Reply.cursor` 返回实际生效值，客户端可检测“ACK 未推进/被拒绝”。

### 未读

```sql
unread = last_seq > ack_seq ? (last_seq - ack_seq) : 0;
```

`ack_seq` 在入群时初始化为 `join_seq`，因此不会把入群前消息算进未读。

### 入群

```sql
join_seq = 当前 last_seq;
ack_seq = join_seq;
read_seq = join_seq;
active = 1;
epoch = epoch + 1;
```

- 入群前消息不可见；
- `delivered_seq` 不受影响；
- 重新入群同样以当前 `last_seq` 为起点。

### 踢人/退群

`active=0`、`epoch+1`；投递时只取 `active=1`；不删除消息。  
被踢/退群后的迟到 ACK 必须 no-op 成功，不能更新 `ack_seq`。

---

## 13. 客户端行为与协议

### 小群

客户端收到 `type=251 Notice`：

- `messages` 非空：直接渲染，按 `seq` 去重；
- `lastSeq` 用于校验/补洞；
- 处理后发 `Ack{groupID, ackSeq}`。

### 大群

客户端收到 `type=251 Notice`：

- 只有 `lastSeq`；
- 打开群或前台时发 `Pull{after=ack_seq}`；
- 分页直到 `hasMore=false`；
- 按 `seq` 排序、按 `seq` 去重；
- 发 `Ack`。

### 超大群

- 可能完全收不到通知；
- 打开群/前台定时 `Pull`；
- 群列表接口返回 `lastSeq`，客户端可显示红点。

### 不变

- 群消息仍按 `seq` 排序；
- 群 ACK 仍是 `ack_seq` 水位；
- 单聊 ACK 仍是 `type=106` 会话水位。

---

## 14. 失败处理

| 故障 | 行为 |
|---|---|
| Kafka 不可用 | 消息写入成功；扫描兜底投递，最坏延迟约 2 秒 + 处理时间 |
| 事件丢失 | 同上 |
| GroupService 实例崩溃 | 租约到期（≤30s）后其他实例接管；Kafka rebalance |
| 投递中崩溃 | `delivered_seq` 未推进，重投；客户端去重 |
| MySQL 不可用 | 不能写入/投递；客户端重试 |
| 网关/推送 Unknown | 不推进或有限重试；扫描兜底 |
| 毒消息 | 超过 `GROUP_MAX_BATCHES` / 重试上限后记录并跳过，避免阻塞整群 |
| 过期 ACK（退群/被踢/群解散） | no-op 成功或 `GROUP_NOT_FOUND`，不写、不断连 |
| ACK 越界（`after > last_seq`） | `INVALID_SEQ`，不写、不断连 |
| ACK 提前（未处理到但 <= last_seq） | 服务端无法识别；靠客户端“只 ACK 连续水位”契约 |
| Redis 不可用 | 只影响在线判断/优化；Pull 走 MySQL |

---

## 15. 配置项

| 环境变量 | 默认 | 说明 |
|---|---|---|
| `GROUP_PUSH_MAX_MEMBERS` | 500 | 小群/大群阈值 |
| `GROUP_NOTIFY_MAX_ONLINE` | 5000 | 超过则不发在线通知 |
| `GROUP_MESSAGE_BATCH` | 50 | 小群单批消息数 |
| `GROUP_PUSH_BATCH` | 200 | 单次 PushBatch 账户数 |
| `GROUP_NOTIFY_INTERVAL_MS` | 1000 | 大群通知合并窗口 |
| `GROUP_SCAN_INTERVAL_MS` | 2000 | DB 扫描间隔 |
| `GROUP_SCAN_WINDOW_SECONDS` | 3600 | 只扫最近活跃群 |
| `GROUP_LEASE_SECONDS` | 30 | 群投递租约 |
| `GROUP_MAX_BATCHES` | 5 | 单轮最多处理批次数 |
| `KAFKA_GROUP_EVENTS_TOPIC` | `group-events` | 事件 topic |
| `KAFKA_GROUP_DELIVERY_GROUP_ID` | `group-delivery` | 消费者组 |

---

## 16. 可观测性

指标：

- `group_delivery_backlog`：`last_seq - delivered_seq` 的群数量/最大值；
- `group_delivery_lag_seconds`：事件 seq 时间到投递时间；
- `group_delivery_batch_size`：单批消息数；
- `group_delivery_notice_total`：推送通知数；
- `group_delivery_retry_total`：租约冲突/推送失败；
- `group_delivery_scan_pending`：扫描发现积压群数。

日志：

- 每次投递：`group_id`, `delivered_seq`, `target`, `members`, `mode(small/large/silent)`, 耗时；
- 租约冲突 debug；
- 推送失败 warn，并记录实例/网关。

---

## 17. 实施步骤

### Phase 1：核心投递

1. `proto/groupchat.proto` 增加 `GroupDeliveryEvent`。
2. `MySQLHandler`：
   - `EnsureGroupDeliverySchema()`：建表 + 回填 + `im_groups.updated_at` 迁移；
   - `ClaimGroupLease(groupID, owner, ttl)`；
   - `ReleaseGroupLease(groupID, owner)`；
   - `GetGroupDelivery(groupID)`；
   - `AdvanceGroupDelivered(groupID, target, owner)`；
   - `MarkGroupNotified(groupID, timestamp)`；
   - `FilterOnlineMembers(members)`（Redis MGET 或 PushBatch 结果）；
   - `PendingGroups(limit, updatedAfter, now)`；
   - `GetGroupMessagesAfter(groupID, after, limit)`（复用 `GroupDeliveryBatch` 可）。
3. `GroupService`：
   - 新增 `GroupDeliveryWorker`（`src/services/group_service/delivery_worker.{h,cpp}`）：
     - `Run()`：扫描循环；
     - `OnKafkaEvent(groupID, seq)`；
     - `DeliverGroup(groupID)`；
     - `FilterOnlineMembers(members)`：Redis MGET `gw:user:{account}`，
       或直接交给 `PushBatch` 返回 NotOnline；
   - 删除 `Deliver()` 的 2 秒全量轮询；
   - 启动 Kafka consumer（topic/consumer group 可配置）。
4. `GroupService` 发送事务写 `updated_at`。
5. 网关 `GroupClient` 改为多实例轮询/失败切换（前置，见 18.3）。
6. 测试：单实例、多实例、Kafka 关闭、崩溃接管。

### Phase 2：大群与拉取

7. 小群 `Notice.messages` 推送；
8. 大群阈值、通知合并、在线人数上限；
9. `Pull` 分页与热点群缓存；
10. 压测 1000/5000 人群。

### Phase 3：超大群（可选）

11. 网关本地 fan-out（Redis channel / 群订阅）；
12. 热点群独立 topic/分片。

---

## 18. 文件级改动清单

### 18.1 数据层

- `src/storage/mysqlhandler.cpp`
  - `InitializeGroups`：建 `im_group_delivery`、回填、`im_groups.updated_at` 迁移；
  - 新增上述投递方法（含 `MarkGroupNotified`）；
  - `Send` 分支更新 `updated_at`。
- `include/storage/mysqlhandler.h`：方法声明。

### 18.2 群服务

- `src/services/group_service/server.cpp`
  - 删除 `Deliver()` 轮询；
  - 启动 `GroupDeliveryWorker`；
  - 发送后 best-effort 发 Kafka。
- `include/services/group_service/server.h`
  - 成员：worker、producer。
- 新增 `src/services/group_service/delivery_worker.h/.cpp`
  - 租约、批次、推送、扫描。
- `src/main.cpp`
  - GroupService 注册方法列表如需增加；
  - 配置读取。

### 18.3 网关前置

- `src/gateway/webserver.cpp`：`discover_Service("GroupService")` 当前只取 `instances(0)`；
- 需要改成多实例轮询/失败切换（建议抽 `ServiceChannel`），否则 GroupService 多实例后，群命令仍打第一台。
- 这是一条独立改造，但在 Phase 1 验收前必须完成。

### 18.4 Proto

- `proto/groupchat.proto`：`GroupDeliveryEvent`；
- `Notice.messages` 已存在，无需新增；
- `Pull/Ack/Read` 已存在，复用。

---

## 19. 测试计划

### 19.1 单元/集成

| 用例 | 期望 |
|---|---|
| 连续发 10 条群消息 | seq 1..10；`delivered_seq` 最终 10 |
| 同一 `(sender, clientID)` 重发 | 不重复分配 seq |
| 两个 GroupService 实例 | 同一群不被并发推进；`delivered_seq` 单调 |
| Kafka 关闭 | 扫描在 ≤5 秒内投递 |
| kill 持有租约的实例 | 其他实例 ≤ 租约+扫描间隔接管 |
| 小群 300 人 | Notice.messages 推送；离线拉取 |
| 大群 1000 人 | 只推 lastSeq；Pull 分页 |
| 超大群 6000 人 | 不推送；Pull 正常 |
| 入群 `join_seq` | 只能拉到入群后的消息 |
| Ack/Read | `ack_seq/read_seq` 只增不减 |
| 低水位迟到 ACK | 先 100 后 80，`ack_seq` 仍为 100 |
| ACK 越界 | `after > last_seq` 返回 `INVALID_SEQ`，`ack_seq` 不变 |
| 退群/被踢后 ACK | no-op 成功，`ack_seq` 不变，连接保持 |
| 群解散后 ACK | `GROUP_NOT_FOUND`，不写、不断连 |
| ACK 提前 | 记录当前行为；客户端只 ACK 连续水位（契约测试） |
| 重复事件 | `delivered_seq` 不回退，重复通知客户端可去重 |

### 19.2 脚本

建议新增：

- `scripts/smoke/group_delivery_test.py`：
  - 启动两个 GroupService；
  - 建群、发 N 条；
  - 断言 `im_group_delivery.delivered_seq`；
  - 断言在线成员收到通知、离线成员可 Pull；
- `scripts/smoke/group_failover_test.sh`：
  - 启动两实例，kill 一个；
  - 等待租约过期；
  - 断言消息最终可拉取、无丢失。

### 19.3 回归

- `scripts/smoke/group_test.py` 必须继续通过；
- `all_test.py`、`drain_test.py` 不受影响。

---

## 20. 验收标准

> 实施完成后逐条勾选；每条必须可复现、可观测。

### A. 数据与写入

- [ ] A1. `im_group_delivery` 表存在，每个群一行；`im_group_outbox` **不存在**。
- [ ] A2. 连发 N 条群消息，`im_group_messages.seq` 为连续的 `1..N`（或从当前 `last_seq+1` 开始连续）。
- [ ] A3. 同一 `(sender, clientID)` 重发不产生新 seq、不产生新消息行。
- [ ] A4. 发送事务只更新 `im_groups.last_seq/updated_at`，不写任何 per-message 投递任务。
- [ ] A5. 已有群回填后 `delivered_seq = last_seq`，上线不会全量重推历史消息。

### B. 投递合并

- [ ] B1. 扫描间隔内落库 N 条消息，`im_group_delivery.delivered_seq` 只推进一次（或按 `GROUP_MESSAGE_BATCH` 分批，批次数 ≤ ceil(N/batch)）。
- [ ] B2. 小群：一次投递携带本批 `messages`，在线成员直接收到；离线成员不推送。
- [ ] B3. 大群：一次投递只发 `lastSeq`，不携带消息正文。
- [ ] B4. 同一群在 `GROUP_NOTIFY_INTERVAL_MS` 内最多一次通知。
- [ ] B5. `delivered_seq` 推进使用快照 `target`，不会跳过投递期间新到的消息（构造并发发送用例验证）。
- [ ] B6. 推送返回 `Unknown` 时 `delivered_seq` 不推进；重试/扫描后仍能补投。
- [ ] B7. 大群在通知合并窗口内不重复推送，但下一轮通知携带最新 `lastSeq`。
- [ ] B8. 因合并窗口 `defer` 时不推进 `delivered_seq`；窗口结束后仍能补发，不丢通知。

### C. 多实例与故障

- [ ] C1. 两个 GroupService 同时运行，同一群不会被两个实例同时推进 `delivered_seq`。
- [ ] C2. 杀掉持有租约的实例，另一个实例在 `GROUP_LEASE_SECONDS + GROUP_SCAN_INTERVAL_MS` 内接管。
- [ ] C3. 停掉 Kafka，消息仍能在 ≤5 秒内通过扫描投递（`delivered_seq` 追上 `last_seq`）。
- [ ] C4. 投递过程中崩溃，`delivered_seq` 不前进；恢复后重投，客户端按 `seq` 去重。
- [ ] C5. Kafka 重复事件不会导致 `delivered_seq` 回退或重复推进。

### D. 群语义

- [ ] D1. 入群后只能拉到 `join_seq` 之后的消息。
- [ ] D2. `Ack` 只更新 `ack_seq`，`Read` 只更新 `read_seq`，两者只增不减；更新条件含 `active=1`。
- [ ] D3. 未读数 = `last_seq > ack_seq ? last_seq - ack_seq : 0`（消息口径）。
- [ ] D4. 踢人/退群后不再投递，重新入群以当前 `last_seq` 为 `join_seq`；其迟到 ACK 为 no-op。
- [ ] D5. 群消息按 `seq` 升序返回；`Pull` 支持 `after/limit/hasMore`。
- [ ] D6. 群解散/成员不存在时 ACK/Read 不写、不断连（`GROUP_NOT_FOUND` 或 no-op）。
- [ ] D7. `after > last_seq` 的 ACK/Read 返回 `INVALID_SEQ`，`ack_seq/read_seq` 不变。
- [ ] D8. 低水位迟到 ACK（100 后 80）不回退；`Reply.cursor` 返回实际生效值。
- [ ] D9. ACK 提前（<= last_seq 但未处理）行为已写入客户端契约：客户端只 ACK 连续水位。

### E. 规模与性能

- [ ] E1. 1000 人群发 10 条：不产生 1000×10 次推送；只产生通知/批量。
- [ ] E2. 6000 人群：在线成员超阈值时不推送，只推进 `delivered_seq`；客户端可 Pull。
- [ ] E3. 群服务从 1 实例扩到 2 实例，吞吐上升，DB 无重复推进。
- [ ] E4. 扫描查询走 `im_groups.updated_at` 索引，`EXPLAIN` 不出现全表扫描。

### F. 回归与可观测性

- [ ] F1. `group_test.py` 通过。
- [ ] F2. `all_test.py`、`drain_test.py` 通过。
- [ ] F3. 日志包含 `group_id/delivered_seq/target/mode/耗时`。
- [ ] F4. 指标 `group_delivery_backlog`、`group_delivery_lag_seconds` 可采集。

### G. 文档

- [ ] G1. 本文档与实际实现一致（表名、配置、行为）。
- [ ] G2. 客户端协议变更说明（251 Notice、Pull/Ack）已同步给客户端。
- [ ] G3. 客户端能区分 `Notice.messages` 非空（小群直接渲染）与只带 `lastSeq`（大群需 Pull）。

---

## 21. 风险与回滚

| 风险 | 缓解 |
|---|---|
| 租约过期导致重复投递 | 客户端按 `seq` 去重；`delivered_seq` CAS |
| 扫描全表慢 | 只扫最近活跃群 + `updated_at` 索引；必要时 Redis pending 集合 |
| 大群通知仍然 O(在线) | 阈值 + 合并 + 静默；Phase 3 网关 fan-out |
| Kafka 消费者 rebalance 抖动 | 分区数 ≥ 3×实例数；处理时间控制在租约内 |
| 历史群无 delivery 行 | 启动时 `INSERT IGNORE ... SELECT` 回填 |

回滚：

- 保留旧 `Deliver()` 代码路径可通过配置开关切回（建议加 `GROUP_DELIVERY_MODE=legacy|watermark`）；
- `im_group_delivery` 表不影响旧逻辑；
- 关闭 Kafka 消费者即可退回轮询。

---

## 22. 附录：与单聊 outbox 的对比

| 项 | 单聊 | 群聊（本方案） |
|---|---|---|
| 可靠性 | per-message outbox | Kafka best-effort + DB 扫描 |
| 投递单位 | 单条消息 | 群 seq 区间 |
| 状态 | `im_outbox` 每消息 | `im_group_delivery` 每群 |
| ACK | 会话水位（106） | 每成员 `ack_seq` |
| 顺序 | 会话 seq | 群 seq |
| 水平扩展 | outbox worker + 网关路由 | Kafka partition + 租约 |

---

## 23. 待确认参数（实施前）

1. `GROUP_PUSH_MAX_MEMBERS` 取 500 还是 1000；
2. 大群通知是否需要“红点”语义；
3. 在线成员判断是否允许一次 MGET 5000 个账号；
4. 是否需要在 Phase 1 就加入 `GROUP_DELIVERY_MODE` 回滚开关；
5. Kafka 分区数按生产实例数确定（建议 32 起）。