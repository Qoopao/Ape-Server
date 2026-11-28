# 单聊消息可靠性与 ACK 设计

> 状态：已实施  
> 权威性：本文是「单聊发送、outbox、每会话 seq、ACK 水位、离线同步」的唯一设计来源  
> 最后核对：2026-11-26 @ `03ed7c2` + 工作区改造  
> 适用代码：src/services/msg_service、src/services/push_service、src/gateway/ws_session.cpp

## 0. 范围

本文覆盖单聊发送、outbox、每会话 seq、Push 与 ACK 水位；群聊见 [group-chat.md](group-chat.md)。单聊链路总览见 [../architecture.md](../architecture.md) §4，数据模型见 [../implementation/schema.md](../implementation/schema.md)。

## 1. 背景

单聊消息的投递链路是：

```
发送方 → MsgService(outbox) → Mongo 落库 → Kafka → PushService → 网关 → 接收方
```

接收方处理完消息后，通过 `type=106` 上报 ACK。ACK 用于：

1. 告诉服务端“这个会话我已经处理到哪个 seq”；
2. 让发送侧的 outbox 任务知道消息已送达，不再重试；
3. 触发 Mongo 投递状态清理（`status=2`）；
4. 作为离线同步之外的“送达水位”。

当前 ACK 已经从“逐条 serverMsgID”改为“按会话水位”，但需求边界还没有写死。本文档把单聊 ACK 的逻辑、协议、数据、异常和验收标准固定下来。

---

## 2. 目标与非目标

### 目标

- 定义单聊 ACK 的唯一语义：**每会话一个连续水位**；
- 明确乱序、重复、提前、过期、越界 ACK 的处理；
- 明确 ACK 与 outbox、Mongo、离线同步的关系；
- 明确客户端必须遵守的契约；
- 给出可验证的验收标准，供实现和测试使用。

### 非目标

- 群聊 ACK（`type=250 Command{Ack}`，按群 `ack_seq`）；
- 已读回执（单聊 `type=241 MarkRead`，更新 `read_seq`）；
- 多设备同步（当前单设备，后登录踢前会话）；
- 逐条消息 ACK；
- 跨会话全局 ACK。

---

## 3. 术语与不变量

| 术语 | 含义 |
|---|---|
| `conversationID` | 单聊会话 ID，格式 `min(uid)_max(uid)`，ASCII |
| `seq` | 单聊会话内稠密序号，双向共用，严格 +1 |
| `delivered_seq` | 某用户在某会话已确认处理到的最大 seq |
| `max_seq` | `im_conversations.max_seq`，会话已提交的最大 seq |
| `ackType` | 1=在线 ACK，2=离线同步后的 ACK |
| `op=1` | 发送任务 |
| `op=3` | ACK 清理任务（更新Mysql的水位+更新Mongo的消息数据状态） |

> Outbox 完成任务后不会自动删除；保留与清理策略见 11.1。

### 不变量

1. 同一会话内 `seq` 从 1 开始、严格 +1（历史数据清空后成立）；
2. `delivered_seq` 只增不减；
3. `delivered_seq <= max_seq`；
4. ACK 只代表“已处理”，不代表“已读”；
5. ACK 不改变消息内容、不删除消息；
6. 同一 `(owner, conversationID)` 只有一行回执；
7. ACK 是幂等的：重复 ACK 不产生额外业务效果。

---

## 4. 端到端流程

### 4.1 角色

| 角色 | 职责 |
|---|---|
| 发送方客户端 | 发消息、收延迟回执 |
| 网关 WSSession | 鉴权、转发、推送、收 ACK |
| MsgService | 分配 seq、outbox、写 Mongo |
| Kafka | 在线通知，不是消息存储 |
| PushService | 消费 Kafka、决定在线/离线、处理 ACK |
| 接收方客户端 | 去重、排序、计算连续水位、发 ACK |
| MySQL | `im_outbox`、`im_conversation_receipts`、会话 seq |
| Mongo | 消息正文、`status` |

### 4.2 在线送达 + ACK（主链路）

假设 A 给 B 发一条消息，B 在线。

| 步骤 | 执行方 | 动作 | 关键数据/状态 |
|---|---|---|---|
| 1 | A 客户端 | `type=101 SendMessageReq{msgs:[...]}` | `clientMsgID`、`recvID=B` |
| 2 | 网关 | 覆盖 `sendID=A`，调用 `MsgService.SendMessages` | 不信任客户端传的 sendID |
| 3 | MsgService | 事务：分配 seq、插入 `im_outbox(op=1, state=0, stage=0)` | `seq`、`serverMsgID` |
| 4 | MsgService | 返回 `SendMessageResp`（可能 `PENDING`） | 只代表已入 outbox |
| 5 | Outbox worker | `phase(true)`：写 Mongo + 更新会话 + `stage=1` | `max_seq=seq` |
| 6 | Outbox worker | `phase(false)`：`PublishMessage` + 检查回执 + Redis 缓存 + Kafka produce | 失败则退避重试 |
| 7 | PushService | 消费 Kafka，查 Redis/Mongo，`MessageDispatchDone` 去重 | 未 ACK 时不跳过 |
| 8 | PushService | `PushMsg` → 网关 `PushFrame` | 在线返回 `DELIVERED` |
| 9 | 网关 | 写 `type=104 SdkWSResp{data=MsgData}` 到 B 的 socket | 入写队列即认为已推送 |
| 10 | B 客户端 | 解析、按 `serverMsgID` 去重、按 `seq` 排序 | 维护本地会话状态 |
| 11 | B 客户端 | 计算连续水位 `upToSeq` | 有缺口不能跳 |
| 12 | B 客户端 | `type=106 AckReq{ackType=1, conversations:[{conversationID, upToSeq}]}` | 可一次带多个会话 |
| 13 | 网关 | `handleAck`：解析、过滤空/`<=0`，一次 RPC `AckConversations` | 8 秒超时 |
| 14 | PushService | `AckConversation`：校验、截断、`GREATEST`、写回执 + `op=3` | 事务提交后返回 |
| 15 | Outbox worker | 处理 `op=3`：Mongo `updateMany(seq<=upToSeq, status=2)` | 按水位批量，不逐条 |
| 16 | Outbox worker | 下一次 `op=1 phase(false)` 查回执，发现覆盖则 `state=2` | 不再重投 Kafka |

### 4.3 ACK 帧示例

B 收到 seq 101、102、103，处理完。下面是逻辑示意，实际 WS 帧的 `data` 是 `AckReq` 的 protobuf 序列化字节：

```json
{
  "type": 106,
  "data": {
    "userID": B,
    "ackType": 1,
    "conversations": [
      { "conversationID": "min(A,B)_max(A,B)", "upToSeq": 103 }
    ]
  }
}
```

服务端：

```sql
-- 1) 截断
SELECT max_seq FROM im_conversations
WHERE owner_account=B AND conversation_id='min_max';  -- 103

-- 2) 水位
INSERT INTO im_conversation_receipts(owner_account,conversation_id,delivered_seq,updated_at)
VALUES (B, 'min_max', 103, <now_ms>)
ON DUPLICATE KEY UPDATE
  delivered_seq=GREATEST(delivered_seq,VALUES(delivered_seq));

-- 3) 清理任务
INSERT INTO im_outbox(operation,dedupe_key,actor_account,peer_account,
                      conversation_id,server_msg_id,seq,payload,...)
VALUES (3,'ack:B:min_max:103',B,B,'min_max','',103,'',...)
ON DUPLICATE KEY UPDATE id=im_outbox.id;
```

### 4.4 迟到/重复 ACK

| 情况 | 服务端行为 |
|---|---|
| 103 后收到 102 | `GREATEST` 保持 103；`op=3(102)` 是无效写（规划优化为跳过） |
| 重复 103 | dedupe key 相同，不新增 `op=3` |
| 103 后收到 110，但 `max_seq=103` | 截断到 103；ACK 无回传，客户端不需要知道；cursor 必须独立于 ACK |
| 会话已删除后收到 ACK | no-op 成功，不写、不断连 |
| ACK 时 104/105 还在 outbox `stage=0` | `max_seq` 仍是 103，只确认到 103；104/105 提交后需再 ACK |

### 4.5 离线

假设 B 离线时 A 发了 101–105。

| 步骤 | 执行方 | 动作 | 状态 |
|---|---|---|---|
| 1 | MsgService | 消息 101–105 已 `phase(true)` 落 Mongo | `max_seq=105` |
| 2 | PushService | Kafka 消费，`PushMsg` 返回 `NOT_ONLINE` | 没有在线会话 |
| 3 | PushService | `ConfirmMessageDispatch` | `state=2, dispatch_done=1`，不再重试 |
| 4 | B 登录 | `type=91`，携带 `cursors` | 可能没有 cursor |
| 5 | 网关 | `syncOffline` 取最近 50 个会话 | `im_conversations` |
| 6 | 网关 | 每个会话取 `seq>cursor` 的最后 20 条 | Mongo |
| 7 | 网关 | 逐条发 `type=200`，最后发 `type=209 SyncSummary` | `ranges/fromSeq/toSeq/hasGap` |
| 8 | B 客户端 | 渲染、去重、计算连续水位 | 有缺口先 Pull |
| 9 | B 客户端 | `type=244 GetSingleChatHistory{afterSeq,limit}` 补洞 | 可选 |
| 10 | B 客户端 | `type=106 ACK{upToSeq}` | 批量/一次性 |
| 11 | PushService | 写回执 + `op=3` | `op=1` 已 `state=2`，ACK 只更新状态 |

关键：

- 离线同步**不等待 ACK**；
- ACK 不是离线拉取的前置条件；
- `op=1` 已因 `dispatch_done=1` 完成，ACK 不再影响发送任务；
- ACK 仍会把 Mongo 的 101–105 标 `status=2`。

### 4.6 重连

```
断线
  → 客户端保留每会话 cursor（只到连续已处理位置）
  → 重连 type=91，携带 cursors
  → 服务端补发 cursor 之后的 200 + 209
  → 客户端按 serverMsgID 去重、按 seq 排序
  → 有缺口走 244
  → 重新计算连续水位并 ACK
```

可能重复收到断线前已推送的消息，客户端必须去重。

### 4.7 一个具体例子

会话 `conv = 100_200`，`max_seq=105`，B 已处理到 103。

```
B 发 ACK upToSeq=103
  → im_conversation_receipts.delivered_seq=103
  → op3(103) → Mongo seq<=103 status=2
  → op1(101..103) 完成

之后 B 收到 104、105，处理完
B 发 ACK upToSeq=105
  → delivered_seq=105
  → op3(105) → Mongo seq<=105 status=2
  → op1(104..105) 完成
```

错误示例：

```
B 只处理到 103，却 ACK 105
  → delivered_seq=105
  → op1(104..105) 认为已送达
  → 若 B 本地 cursor 也推到 105，104/105 永远不会再拉
```

因此客户端只能 ACK **连续已处理水位**。

### 4.8 ACK 与已读、离线同步的边界

| 行为 | ACK 是否影响 |
|---|---|
| 未读数 | 不影响（单聊未读看 `read_seq`/会话未读，不看 ACK） |
| 已读回执 | 不影响（`type=241 MarkRead`） |
| 离线补发 | 不影响（看 cursor） |
| 发送任务完成 | 影响（水位覆盖即完成） |
| Mongo `status=2` | 影响（`op=3`） |

---

## 5. 协议

### 5.1 WebSocket 帧

```proto
message ConversationAck {
  string conversationID = 1; // 必填，min_max
  int64 upToSeq = 2;         // 必填，>0
}

message AckReq {
  uint64 userID = 1;                   // 服务端以会话账号为准
  repeated ConversationAck conversations = 2;
  int32 ackType = 3;                   // 1=在线 2=离线
}

// SdkWSResp
int32 type = 106;
```

- 一帧可以带多个会话；
- 无 ACK 响应帧：ACK 是 fire-and-forget，客户端不等待、不依赖 ACK 结果推进 cursor；
- 客户端不需要逐条消息 ACK。

### 5.2 gRPC

```proto
message AckConversation {
  string conversationID = 1;
  int64 upToSeq = 2;
}

message AckConversationsReq {
  uint64 userID = 1;
  repeated AckConversation conversations = 2;
  int32 ackType = 3;
}

message AckConversationsResp {}

rpc AckConversations(AckConversationsReq) returns (AckConversationsResp);
```

### 5.3 校验

网关：

- `type=106`，`AckReq` 解析失败 → 忽略；
- `ackType` 不是 1/2 → 忽略；
- `conversations` 为空 → 忽略；
- 单项 `conversationID` 为空或 `upToSeq <= 0` → 过滤；
- 过滤后为空 → 忽略；
- 一次 RPC 发送整帧。

服务端：

- `conversationID` 必须能解析为 `digits_digits`；
- `owner` 必须是其中一方；
- 否则按“过期/非参与者 ACK”处理（见 7.4）。

---

## 6. 服务端处理规则

### 6.1 `AckConversation(owner, conversationID, upToSeq)`

1. 基础校验：`owner != 0`、`conversationID` 非空、`upToSeq > 0`；
2. 解析 `conversationID`：
   - 不是 `digits_digits` → 返回 `INVALID_ARGUMENT`（协议错误）；
   - `owner` 不是其中一方 → **no-op 成功**；
3. 查 `im_conversations` 中 `(owner, conversationID)` 的 `max_seq`：
   - 行不存在（会话已删除/历史清理）→ **no-op 成功**；
4. `applied = min(upToSeq, max_seq)`：
   - `applied <= 0` → no-op 成功；
5. 事务：
   - upsert `im_conversation_receipts(owner, conversationID, delivered_seq)`；
   - `delivered_seq = GREATEST(delivered_seq, applied)`；
   - 插入 `op=3` outbox，dedupe key `ack:{owner}:{conversationID}:{applied}`；
6. 返回成功。

### 6.2 事务与并发

- `INSERT ... ON DUPLICATE KEY UPDATE` 持有主键行锁；
- 并发 ACK 串行化；
- `GREATEST` 保证最终值等于最大水位；
- 多实例无需亲和，任意 PushService 实例处理结果一致。

### 6.3 `op=3`

- `actor = owner`、`peer = owner`、`conversation_id = conversationID`、`seq = applied`；
- worker 执行 `MarkConversationDeliveredUpTo(owner, conversationID, applied)`；
- Mongo 条件：`recvID=owner`、`convID=conversationID`、`isGroupMsg=false`、`seq<=applied`、`status!=2`；
- 批量置 `status=2`；
- 任务完成置 `state=2`；
- 失败重试幂等。

### 6.4 `op=1` 完成条件

- 发送任务 `phase(false)` 检查：

```sql
SELECT 1 FROM im_conversation_receipts
WHERE owner_account=? AND conversation_id=? AND delivered_seq>=?
```

- 满足则直接完成，不再依赖 Kafka 投递；
- `MessageDispatchDone` 同样检查：
  - outbox `state=3` 或 `dispatch_done=1`；
  - 或 `delivered_seq >= seq`。

---

## 7. 乱序、重复、提前、过期

### 7.1 低水位迟到

```
ACK 100 → delivered_seq=100
ACK 80  → GREATEST 保持 100
```

- 不回退；
- 幂等语义正确；
- **已实现**：`applied <= 当前 delivered_seq` 时直接返回，不写回执、不插 `op=3`（见 13.2 / G2）；
  判定与 `max_seq` 读取都在同一事务内、先 `SELECT ... FOR UPDATE` 锁回执行，并发 ACK 被串行化（6.2）。


### 7.2 重复 ACK

- 同一 `applied`：dedupe key 相同，不新增 `op=3`；
- 回执行 `GREATEST` 不变；
- 无业务影响。

### 7.3 高水位提前

```
客户端只处理到 90，却 ACK 100
```

- 服务端无法识别；
- `delivered_seq=100`；
- `op=1` 认为 91–100 已送达；
- `MessageDispatchDone` 会跳过后续推送；
- Mongo 把 91–100 标 `status=2`；
- **但**登录同步 `LatestAfterMessages` 不看 `status`，消息仍可被拉到；
- 如果客户端同时把本地 cursor 推到 100，就不会再拉 91–100，造成客户端侧缺失。

**结论：服务端只保证不回退，不保证“ACK 对应真实处理”；客户端必须只 ACK 连续水位。**

### 7.4 过期/非参与者 ACK

- 会话已删除 → no-op 成功；
- owner 不是会话参与者 → no-op 成功；
- 不写回执、不插 `op=3`、不断连；
- 目的：避免好友删除/历史清理后的迟到 ACK 把连接踢掉。

### 7.5 越界 ACK

- `upToSeq > max_seq` → 截断到 `max_seq`；
- 不确认未提交消息；
- 设计上不回传实际生效值（见 13.1）：客户端 cursor 不依赖 ACK，截断不影响后续同步。

### 7.6 提交竞态

```
ACK 100 到达时，消息 99/100 还在 outbox stage=0
```

- `max_seq` 可能仍是 98；
- `applied = 98`；
- 99/100 之后提交，不会被这次 ACK 覆盖；
- 客户端收到 99/100 后需要再次 ACK；
- 服务端不报错，避免误伤。

---

## 8. ACK 与离线同步的关系

| 项 | ACK | 离线同步 |
|---|---|---|
| 触发 | 客户端主动 | 登录/重连 |
| 依据 | `upToSeq` | 客户端 `cursors` |
| 数据源 | `im_conversation_receipts` | Mongo `msg` |
| 是否过滤 status | — | **不按 status 过滤** |
| 是否阻塞同步 | 否 | 否 |
| 失败影响 | 回执滞后/重试 | 209 PARTIAL/重试 |

关键：

- ACK 乱序不会让离线消息“消失”；
- ACK 无回传也不影响同步：客户端不需要知道 ACK 是否成功；
- 真正会让离线消息跳过的是**客户端 cursor 提前**；
- 客户端应把 cursor 与“连续已处理水位”保持一致，且 cursor 不依赖 ACK 结果。

### 8.1 A 方案：服务端只保证“可恢复”，不保证“连续可见”

已确认采用 A 方案：

> **服务端不改变写入顺序，不做可见水位，也不保证同步结果一定连续；只保证消息不丢、按 cursor 能拉到。**

#### 服务端必须做

1. **ACK 不删除消息**
   - ACK 只更新 `im_conversation_receipts` 和 `op=3`；
   - 消息保留在 Mongo，消息历史清理与 ACK 无关。

2. **同步/Pull 按 cursor 返回**
   - `LatestAfterMessages`、`GetSingleChatHistory` 只看 `cursor/afterSeq`；
   - **不按 `status`、不按 `delivered_seq` 过滤**；
   - 这样即使 ACK 提前把消息标成 `status=2`，客户端仍能重新拉到。

3. **ACK 静默截断、不回传**
   - `upToSeq > max_seq` 时只记 `max_seq`；
   - 网关不回 ACK 结果帧；
   - 客户端不依赖 ACK 结果。

4. **Pull 返回 `minSeq/deletedSeq`（规划）**
   - 用于区分“104/105 还没提交”和“104/105 已永久删除”；
   - 没有这个字段，客户端遇到永久缺口会一直重试。

#### 服务端明确不做

- 不做写侧顺序门卫；
- 不做 `visible_seq` 可见水位；
- 不做读侧“只返回连续前缀”；
- 不返回 ACK 结果；
- 不强制校验客户端是否真的持久化/连续处理。

#### 客户端可信假设

- 本期假设客户端可信：会先本地持久化、只推进连续 cursor、发现 gap 主动 Pull；
- 客户端约束本期只写进文档，**不做服务端强制**；
- 如果客户端被修改，最多影响它自己（ACK 过大、重复 Pull、自己跳过消息），
  不影响其他用户，也不破坏服务端数据；
- 服务端仍保留基本边界：ACK 只能操作自己的会话、不能越权改别人的回执。

#### 106 先落库、104/105 后落库

这是 A 方案接受的临时缺口：

```
cursor=103
Mongo 暂时只有 106
同步/Pull 返回 106
客户端 buffer 106，不推进 cursor
客户端 Pull(103) 仍只返回 106
退避后重试
104/105 落库后，Pull(103) 返回 104、105、106
客户端补齐，cursor=106
```

代价是 106 会晚一点上屏；**不是丢消息**。

---

## 9. 客户端契约（可信客户端，后续版本强制）

> 本期服务端不强制这些约束；以下是客户端实现必须遵守的契约，供后续版本做强制/检测。

### 9.1 ACK 规则

1. 按 `(conversationID, seq)` 排序；
2. 按 `serverMsgID` 去重；
3. 只 ACK **连续已处理水位**；
4. ACK 是 fire-and-forget：不等待响应、不依赖响应、不把 ACK 结果写 cursor；
5. ACK 失败/重连后重发；
6. 断线前尽量 ACK 一次（可选）。

### 9.2 客户端主动丢消息检测

客户端维护：

```
contiguousSeq：本地已持久化且连续处理到的 seq
maxSeenSeq：本地见过的最大 seq
buffer：seq > contiguousSeq+1 的乱序消息
```

收到消息：

```
if seq <= contiguousSeq: 丢弃（重复）
elif seq == contiguousSeq+1:
    contiguousSeq = seq
    flush buffer 中连续的后续消息
else:
    buffer[seq] = message
    标记 gap
```

发现 gap：

1. 立即 `GetSingleChatHistory{afterSeq=contiguousSeq, limit=100}`；
2. 逐条处理返回的 `messages`，直到 `hasMore=false`；
3. 如果返回为空且 `hasMore=false`：
   - `minSeq/deletedSeq` 表明是永久删除 → 可以跳过；
   - 否则**不能推进 cursor**，退避后重拉；
4. 只有缺口补齐后，才推进 `contiguousSeq`。

### 9.3 定期对账

- 登录/重连：209 的 `ranges/hasGap` 或 `toSeq` 与本地 `contiguousSeq` 比较；
- 前台恢复：对当前会话 244，`afterSeq=contiguousSeq`；
- 会话列表：用 240 的 `maxSeq/minSeq` 判断本地是否落后；
- 发现 `serverMaxSeq > contiguousSeq` 就拉，不依赖 ACK。

### 9.4 示例

```
收到 1,2,5
  → contiguous=2，buffer={5}
  → Pull(after=2) 返回 3,4
  → contiguous=5，flush 5

收到 1,2,3，但只渲染了 1,2
  → contiguous=2
  → 不能 ACK 3
```

### 9.5 永久空洞

删除/撤回/好友删除清理会产生永久缺口：

- 客户端必须能区分“临时未提交”和“永久删除”；
- 依赖 Pull 返回的 `minSeq/deletedSeq`；
- 不能把任何缺口都当永久，否则会跳过尚未提交的消息；
- 也不能无限等待永久缺口，否则 cursor 永远不前进。

### 9.6 本期不做

- 服务端强制校验客户端 cursor/持久化；
- 区间/位图 ACK；
- 逐条 ACK；
- 客户端可信证明/签名。

这些留到后续版本；本期服务端只保证“消息在、能拉到”。

### 9.7 禁止（客户端自查）

- 用“见过的最大 seq”直接 ACK；
- 用 ACK 结果推进 cursor；
- 用到达时间排序；
- 把 ACK 当作已读；
- 对群会话发 `type=106`；
- ACK `upToSeq<=0`；
- 发现 gap 却不 Pull，只等推送。

---

## 10. 失败与重试

| 故障 | 行为 |
|---|---|
| 网关 → PushService 超时 | 网关 `onDisconnect`，客户端重连/重发 |
| PushService 返回 `UNAVAILABLE` | 网关断连；客户端重试 |
| PushService 返回 `INVALID_ARGUMENT` | 协议错误；网关断连；客户端修正 |
| MySQL 不可用 | ACK 失败；客户端重试 |
| Redis 不可用 | 不影响 ACK；只影响在线推送 |
| `op=3` 失败 | outbox 退避重试；水位已持久化 |
| `op=1` 未收到 ACK | 5 秒后重投；收到 ACK 后完成 |
| 进程崩溃 | 水位已提交则保留；未提交则客户端重发 |

---

## 11. 数据模型

```sql
-- 会话级 seq（已存在）
im_conversation_seq (
  conversation_id VARCHAR(64) PRIMARY KEY,
  next_seq BIGINT UNSIGNED NOT NULL DEFAULT 0
);

-- ACK 水位（已存在）
im_conversation_receipts (
  owner_account BIGINT UNSIGNED NOT NULL,
  conversation_id VARCHAR(64) NOT NULL,
  delivered_seq BIGINT NOT NULL DEFAULT 0,
  updated_at BIGINT NOT NULL,
  PRIMARY KEY(owner_account, conversation_id)
);

-- op=3 任务（复用 im_outbox）
im_outbox.operation = 3
im_outbox.dedupe_key = 'ack:{owner}:{conversationID}:{seq}'
```

- 回执行数 = O(用户 × 会话)，不是 O(消息数)；
- `delivered_seq` 不是 `read_seq`，不影响未读数；
- 回执行不随会话删除清理（保留水位，避免重建会话后回退）；
- 可选：对长期不活跃会话做归档/清理（非本期）。

### 11.1 Outbox 保留与清理

**只删已完成任务，未完成永不删。**

| operation | 删除范围 | 保留期 | 配置 |
|---|---|---|---|
| `op=1` 发送任务 | `state IN (2,3)` | 7 天 | `OUTBOX_RETENTION_DAYS` |
| `op=2` 好友清理 | `state IN (2,3)` | 7 天 | `OUTBOX_FRIEND_RETENTION_DAYS` |
| `op=3` ACK 清理 | `state IN (2,3)` | 1 天 | `OUTBOX_ACK_RETENTION_DAYS` |
| 任意 | `state = 0/1` | **永不删除** | — |

删除 SQL：

```sql
DELETE FROM im_outbox
WHERE state IN (2,3)
  AND operation = ?
  AND updated_at < ?
ORDER BY id
LIMIT ?;
```

执行方式：

- 每个 MsgService 实例后台线程，默认每 10 分钟执行一次；
- 每批 `OUTBOX_CLEANUP_BATCH=1000`，每轮最多 `OUTBOX_CLEANUP_MAX_BATCHES=10` 批；
- 多实例同时 DELETE 是幂等的（同一行只有一个实例删到），后续可选 leader lock 减少空转；
- 需要索引 `outbox_retention(state, operation, updated_at, id)` 避免全表扫描；
- 未完成/租约中的任务永远不会被删。

取舍与约束：

- `op=1` 保留 7 天等于 `clientMsgID` 的幂等窗口；超过 7 天的重试会被当成新消息、重新分配 seq；
- `state=3`（好友删除取消）也保留 7 天，避免旧消息被重放；
- 本策略**不清理 payload**，所以 7 天内完成行仍占 `MEDIUMTEXT` 空间；
- `DELETE` 不立即回收 InnoDB 磁盘；数据量大时后续再做 `created_at` 分区 / `DROP PARTITION`；
- **已实现**（11.1 保留期清理）：
  - `MySQLHandler::PurgeCompletedOutbox`：`DELETE ... WHERE state IN (2,3) AND operation=? AND updated_at<? ORDER BY id LIMIT ?`，按 `maxBatches` 轮询，返回删除行数；
  - `OutboxWorker::RunCleanup`：每个 MsgService 实例一个后台协程，启动即跑一轮，之后按 `OUTBOX_CLEANUP_INTERVAL_MS` 执行；
  - 索引 `outbox_retention(state,operation,updated_at,id)` 建表时创建，老库在 `InitializeOutbox` 启动时幂等补建；
  - 清理失败只告警并等下一轮，行和状态都不变；`state=0/1` 的行永不删除。


配置：

| 环境变量 | 默认 | 说明 |
|---|---|---|
| `OUTBOX_RETENTION_DAYS` | 7 | op=1 完成/取消行保留天数 |
| `OUTBOX_FRIEND_RETENTION_DAYS` | 7 | op=2 保留天数 |
| `OUTBOX_ACK_RETENTION_DAYS` | 1 | op=3 保留天数 |
| `OUTBOX_CLEANUP_BATCH` | 1000 | 单条 DELETE 的 LIMIT |
| `OUTBOX_CLEANUP_MAX_BATCHES` | 10 | 每轮每 operation 最多批数 |
| `OUTBOX_CLEANUP_INTERVAL_MS` | 600000 | 清理间隔 |

---

## 12. 现状与目标差距

| 编号 | 项目 | 现状 | 目标 |
|---|---|---|---|
| G1 | ACK 响应 | `AckConversationsResp` 为空 | **不实现回传**：ACK 为 fire-and-forget，客户端不依赖响应 |
| G2 | 低水位 ACK 无效写 | **已实现**：不写、不插 `op=3` | 保持（ACK-033 断言） |

| G3 | 过期 ACK no-op | **已实现** | 保持 |
| G4 | 越界截断 | 已实现 | 保持 |
| G5 | 非参与者 no-op | **已实现** | 保持 |
| G6 | 乱序防护 | 仅 `GREATEST` | 保持；客户端可信，不做服务端强制 |
| G7 | 群 ACK | 独立链路 | 不在本文档范围 |
| G8 | Pull 区分永久空洞 | 无 `minSeq/deletedSeq` | 规划：Pull 返回 `minSeq/deletedSeq` |

---

## 13. 待确认问题

### 13.1 为什么不回传 ACK 结果

已确认：**ACK 不返回任何结果帧**。

原因：

- 消息不丢的兜底是“Mongo 持久化 + 客户端 cursor 同步”，不是 ACK 结果；
- 如果返回 `appliedSeq`，客户端容易把 cursor 和 ACK 耦合，ACK 失败/被截断时反而卡住；
- ACK 只用于服务端侧的回执和 outbox 加速，成功与否不应影响客户端本地 cursor；
- 客户端只需要“本地持久化成功后推进 cursor”，然后异步 ACK；ACK 丢了、被截断、重复都靠重连同步兜底。

因此：

- 服务端静默截断 `upToSeq` 到 `max_seq`；
- 网关不发送 ACK 响应帧；
- 客户端不等待、不解析 ACK 响应；
- 服务端只记录 clamped 日志/指标，用于发现客户端异常，不回传。

### 13.2 低水位 ACK 是否跳过 op=3

已实现：是。事务内先锁回执行，再读当前 `delivered_seq`：


```sql
SELECT delivered_seq FROM im_conversation_receipts
WHERE owner_account=? AND conversation_id=?;
```

`applied <= delivered_seq` → 直接返回，不插任务。

### 13.3 `ackType` 1/2 是否需要不同语义

现状：两者完全一致。
建议：保留字段用于统计/客户端语义，服务端不区分。

### 13.4 单帧 ACK 会话数上限

已实现：`<= 100`，超过时网关按 100 个/批拆成多个 `AckConversations` RPC（`src/gateway/ws_session.cpp` `handleAck`），每个会话的处理结果不变。


### 13.5 ACK 超时/重试

现状：网关失败即断连。
建议：保持；客户端重连后重新 ACK，服务端幂等。

---

## 14. 验收标准

> 执行记录见 §16；`[x]` 表示已由 `scripts/smoke/ack_test.py` 自动断言。

### A. 基本

- [x] A1. 单会话 ACK 后 `delivered_seq == upToSeq`。
- [x] A2. 多会话 ACK 一帧处理，互不影响。
- [x] A3. `ackType=1/2` 均可成功。
- [x] A4. 重复 ACK 不新增 `op=3` 行。

### B. 边界

- [x] B1. `upToSeq=0`/负数被网关过滤，不写。
- [x] B2. `upToSeq > max_seq` 截断到 `max_seq`。
- [x] B3. 会话 `max_seq=0` 时 ACK no-op。
- [x] B4. `conversationID` 格式非法 → `INVALID_ARGUMENT`，不写。
- [x] B5. `owner` 非参与者 → no-op，不写、不断连。
- [x] B6. 会话已删除 → no-op，不写、不断连。
- [x] B7. 群会话 ID 走 106 → 不写（按非法/非参与者处理）。

### C. 乱序/重复

- [x] C1. ACK 100 后 ACK 80：`delivered_seq` 仍 100。
- [x] C2. ACK 80 后 ACK 100：最终 100。
- [x] C3. 并发 ACK 100/80：最终 100，无回退。
- [x] C4. 低水位 ACK 不新增 `op=3`（目标 G2）。
- [x] C5. 高水位提前 ACK：`delivered_seq` 到 100，但登录同步仍能拉到 91–100（服务端不按 status 过滤）。

### D. 与投递/离线

- [x] D1. ACK 后对应 `op=1` 任务完成，不再重投。
- [x] D2. ACK 后 Mongo 对应消息 `status=2`。
- [x] D3. 未 ACK 的离线消息登录仍可拉到。
- [x] D4. ACK 乱序不改变离线 cursor 行为。
- [x] D5. 客户端 cursor 提前时，服务器无法补救（文档明确）。
- [ ] D6. 客户端收到 1,2,5 时能检测 gap，并 Pull 3,4；补齐后才 ACK 5。
- [ ] D7. ACK 无回传：客户端不等待响应，cursor 不依赖 ACK 结果。
- [ ] D8. 209 `hasGap/toSeq` 或 240 `maxSeq` 落后时，客户端主动 Pull。
- [x] D9. ACK 提前把消息标成 `status=2` 后，客户端按旧 cursor 仍能 Pull 到这些消息。
- [ ] D10. Pull 返回 `minSeq/deletedSeq` 后，客户端能区分临时缺口和永久删除。

> D6–D8 属于客户端契约（`CLIENT-*` 用例），本期服务端不强制；D10 依赖规划项 G8
> （`GetSingleChatHistoryResp` 增加 `minSeq/deletedSeq`），本期未实现。

### E. 失败

- [x] E1. PushService 不可用：网关断连，客户端重连后可重发（`--faults` 的 ACK-050）。
- [ ] E2. MySQL 不可用：ACK 失败，不产生半写（停库会破坏共享测试环境，未自动化）。
- [ ] E3. `op=3` 失败：退避重试，最终 `status=2`（outbox 重试机制，未单独注入）。
- [x] E4. 发送任务未提交时 ACK：截断，不报错（ACK-035）。

### F. 非功能

- [ ] F1. 100 个会话一帧在 100ms 内完成（本地基准）。
      > 实测端到端（帧进网关 → 100 次独立 MySQL 事务提交完成）多次运行为 424ms～1.7s，
      > 差异来自机器负载和 outbox worker 并发处理这 100 条 `op=3`；网关侧是
      > fire-and-forget，只等待 RPC 派发，该指标无法从客户端侧单独测量，因此记为 WARN 而非通过。
      > 若要压到 100ms 以内，需要把 ACK 的逐会话事务改为批量提交（本期未做）。


- [x] F2. 1000 条消息/会话逐步 ACK，`op=3` 行数 ≤ 水位推进次数（实测 1000 行 / 1000 次推进）。
- [x] F3. 回执行数不随消息数增长（ACK-062：同一会话多水位 ACK 后回执仍 1 行）。

### H. Outbox 清理

- [x] H1. `state IN (2,3)` 且超过保留期的 op=1 行被删除。
- [x] H2. `state IN (2,3)` 且超过 1 天的 op=3 行被删除。
- [x] H3. `state=0/1` 的行永远不会被删除。
- [x] H4. 删除按 `operation + updated_at + LIMIT` 分批，不产生长事务。
- [ ] H5. 多实例同时清理不报错、不误删（测试环境单 MsgService 实例，未验证）。
- [x] H6. 7 天内同一 `clientMsgID` 重试仍命中旧行（幂等窗口）。

## 15. 参考代码

- 网关：`src/gateway/ws_session.cpp` `handleAck`
- PushService：`src/services/push_service/server.cpp` `AckConversations`
- MySQL：`src/storage/mysqlhandler.cpp` `AckConversation`
- Outbox：`src/storage/mysqlhandler.cpp` `ProcessOutboxTask`
- Mongo：`src/storage/mongohandler.cpp` `MarkConversationDeliveredUpTo`
- 协议：`proto/sdkws.proto` `AckReq`、`proto/push.proto` `AckConversations`

---

## 16. 本期实现与验证记录

### 16.1 代码改动

| 项 | 位置 | 说明 |
|---|---|---|
| G2 低水位无效写 | `src/storage/mysqlhandler.cpp` `AckConversation` | 事务内先 `SELECT delivered_seq ... FOR UPDATE` 锁回执行，`applied <= delivered_seq` 时直接返回，不写回执、不插 `op=3`；同时把 `max_seq` 读取放进同一事务，并发 ACK 串行化 |
| conversationID 校验 | `src/storage/mysqlhandler.cpp` `parseConversationID` | 严格 `digits_digits`（拒绝 `abc`、`1_2_3`、`_`、带空格/符号），非法返回 `INVALID_ARGUMENT`，网关按 10 节断连 |
| 单帧会话数上限 | `src/gateway/ws_session.cpp` `handleAck` | 过滤后按 100 个/批拆成多个 `AckConversations` RPC，语义不变（13.4） |
| H 节 Outbox 清理 | `src/storage/mysqlhandler.cpp` `PurgeCompletedOutbox`、`src/services/msg_service/outbox_worker.cpp` `RunCleanup` | 每个 MsgService 实例一个后台协程；启动即跑一轮，之后按间隔执行；只删 `state IN (2,3)` 且超过保留期的行，`DELETE ... ORDER BY id LIMIT` 分批 |
| 清理索引 | `src/storage/mysqlhandler.cpp` `InitializeOutbox` | 建表带 `outbox_retention(state,operation,updated_at,id)`，老库启动时幂等补建 |
| 配置 | `OUTBOX_RETENTION_DAYS`、`OUTBOX_FRIEND_RETENTION_DAYS`、`OUTBOX_ACK_RETENTION_DAYS`、`OUTBOX_CLEANUP_BATCH`、`OUTBOX_CLEANUP_MAX_BATCHES`、`OUTBOX_CLEANUP_INTERVAL_MS` | 默认值与 11.1 一致；清理线程使用 250ms 分片睡眠，不阻塞停止 |

### 16.2 未实现（保持规划）

- G8：`GetSingleChatHistoryResp` 增加 `minSeq/deletedSeq`，供客户端区分“临时缺口/永久删除”（D10、CLIENT-005）。
- 多实例并发清理（H5）的自动化验证：测试环境只起一个 MsgService 实例。

### 16.3 测试

```bash
# 1) 起 ACK 需要的服务（backbon/auth/msg/push/gw-1/gw-2，并缩短清理间隔）
bash scripts/smoke/start_ack.sh

# 2) 全量验收（默认用构造数据，不依赖 Kafka）
python3 scripts/smoke/ack_test.py --json /tmp/ack-results.json

# 3) 故障注入（会停止并重启 PushService）
python3 scripts/smoke/ack_test.py --faults --case ACK-050

# 4) 真实发送链路（慢，依赖 Kafka）
python3 scripts/smoke/ack_test.py --e2e --case ACK-001 --case ACK-041
```

最近一次执行（2026-11-24，本机 5.8G/16C，MariaDB + MongoDB + Redis 本机进程，无 Kafka）：

```
合计 44 个用例 / 65.3s：{'PASS': 37, 'SKIP': 6, 'WARN': 1, 'FAIL': 0}
指标：ACK-018.elapsed_ms=1473, ACK-061.op3_rows=1000, outbox.cleanup_active=true
明细：docs/testing/results/ack-2026-11-24.json
```

- SKIP：ACK-050（默认不注入；`--faults` 单独执行通过）、ACK-051、ACK-053、ACK-054、ACK-072（客户端契约）、OUTBOX-005（单实例）。
- WARN：ACK-060（端到端 1473ms，多次运行区间 424ms～1.7s，见 F1 说明）。
- 回归：`seq_test.py`、`group_test.py`、`all_test.py` 全部通过。