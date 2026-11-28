# 单聊与 ACK 测试用例

> 状态：现状  
> 权威性：本文是「单聊发送/ACK/离线同步测试用例与边界」的唯一来源  
> 最后核对：2026-11-26 @ `03ed7c2` + 工作区改造  
> 适用代码：scripts/smoke/ack_test.py、test/

> 配套需求：`docs/design/messaging.md`  
> 范围：单聊 `type=106` ACK，不含群聊 ACK  
> 标记说明：`[现状]` 当前代码应通过；`[目标]` 需求确定后实现再通过

---

## 1. 测试环境

| 项 | 要求 |
|---|---|
| 服务 | backbon、auth、msg、push、gateway（单实例即可；并发用例可加 gateway-2） |
| 存储 | MySQL、MongoDB、Redis、etcd |
| Kafka | 在线链路用例需要；离线/ACK 核心用例可不依赖 Kafka |
| 客户端 | Python WebSocket 脚本（复用 `scripts/smoke/group_test.py` 的注册/登录） |
| 查询 | `mariadb` CLI、`pymongo` |

### 1.1 数据准备

```sql
-- 两个已注册用户 A、B
-- 建立好友关系
INSERT IGNORE INTO friends(owner_account, friend_account, created_time)
VALUES (A,B,0),(B,A,0);

-- conversationID
-- min(A,B) + '_' + max(A,B)
```

### 1.2 关键观测点

```sql
-- 水位
SELECT owner_account, conversation_id, delivered_seq
FROM im_conversation_receipts
WHERE owner_account=? AND conversation_id=?;

-- ACK 清理任务
SELECT COUNT(*) FROM im_outbox
WHERE operation=3 AND conversation_id=?;

-- 发送任务
SELECT id, seq, state, stage, dispatch_done
FROM im_outbox
WHERE operation=1 AND conversation_id=? ORDER BY id;

-- 会话
SELECT owner_account, max_seq FROM im_conversations
WHERE conversation_id=?;
```

```python
# Mongo 投递状态
db["IM-System"]["msg"].count_documents(
    {"recvID": B, "convID": conv, "seq": {"$lte": upTo}, "status": 2})
```

---

## 2. 用例总览

| ID | 类型 | 标题 | 期望 |
|---|---|---|---|
| ACK-001 | 基础 | 单会话 ACK | 水位=upToSeq |
| ACK-002 | 基础 | 一帧多会话 | 各自独立更新 |
| ACK-003 | 基础 | ackType 1/2 | 均成功，行为一致 |
| ACK-004 | 基础 | 重复 ACK | 不新增 op=3 |
| ACK-010 | 边界 | upToSeq=0/负数 | 网关过滤，不写 |
| ACK-011 | 边界 | upToSeq > max_seq | 截断到 max_seq |
| ACK-012 | 边界 | max_seq=0 | no-op 成功 |
| ACK-013 | 边界 | 非法 conversationID | INVALID_ARGUMENT，不写 |
| ACK-014 | 边界 | 非参与者 | no-op 成功，不断连 |
| ACK-015 | 边界 | 会话已删除 | no-op 成功，不断连 |
| ACK-016 | 边界 | 群 ID 走 106 | 不写，不断连 |
| ACK-017 | 边界 | 一帧重复会话 | 最终取最大，幂等 |
| ACK-018 | 边界 | 100 个会话一帧 | 全部处理 |
| ACK-019 | 边界 | 超大 upToSeq | 截断，不溢出 |
| ACK-020 | 边界 | 空 conversations | 忽略，不报错 |
| ACK-030 | 乱序 | 100 后 80 | 水位保持 100 |
| ACK-031 | 乱序 | 80 后 100 | 最终 100 |
| ACK-032 | 并发 | 两实例并发 ACK | 最终 100，无回退 |
| ACK-033 | 乱序 | 低水位不新增 op=3 | `[目标]` 不新增 |
| ACK-034 | 提前 | ACK 100 未处理 91–100 | 水位 100，同步仍可拉到 |
| ACK-035 | 竞态 | ACK 时消息未提交 | 截断，不报错 |
| ACK-040 | 投递 | ACK 后 op=1 完成 | state=2 |
| ACK-041 | 投递 | ACK 后 Mongo status=2 | 范围内全部置 2 |
| ACK-042 | 离线 | 未 ACK 离线消息 | 登录可拉到 |
| ACK-043 | 离线 | ACK 乱序不影响 cursor | 209 行为不变 |
| ACK-044 | 重连 | 重连后重复 ACK | 幂等 |
| ACK-050 | 失败 | PushService 不可用 | 网关断连，重连可重发 |
| ACK-051 | 失败 | MySQL 不可用 | 不产生半写 |
| ACK-052 | 失败 | op=3 失败 | 重试后 status=2 |
| ACK-053 | 失败 | ACK RPC 超时 | 断连/重试 |
| ACK-054 | 失败 | Redis 不可用 | ACK 不受影响 |
| ACK-060 | 性能 | 100 会话一帧 | <100ms（本地基准） |
| ACK-061 | 性能 | 1000 条逐步 ACK | op=3 行数 ≤ 推进次数 |
| ACK-062 | 容量 | 回执行数 | O(会话)，不随消息数增长 |
| ACK-070 | 回归 | 209 登录同步 | 不受 ACK 影响 |
| ACK-071 | 回归 | 244 拉历史 | 不受 ACK 影响 |
| ACK-072 | 回归 | 104/200 去重 | 按 serverMsgID 去重 |

---

## 3. 基础用例

### ACK-001 单会话 ACK `[现状]`

**前置**

- A、B 是好友；
- A 给 B 发 5 条，B 已收到；
- `max_seq=5`。

**步骤**

1. B 发 `AckReq{ackType=1, conversations=[{conv, upToSeq=5}]}`；
2. 等待 1 秒；
3. 查 `im_conversation_receipts`。

**期望**

- `delivered_seq=5`；
- `im_outbox` 有一条 `operation=3`，dedupe 为 `ack:B:conv:5`；
- B 的连接保持。

### ACK-002 一帧多会话 `[现状]`

**前置**

- B 与 A、C 各有一个会话，均有未 ACK 消息。

**步骤**

- 一帧携带两个会话的 `upToSeq`。

**期望**

- 两个 `delivered_seq` 各自更新；
- 两条 `op=3`（或按水位各一条）；
- 互不影响。

### ACK-003 ackType 1/2 `[现状]`

**步骤**

- 分别用 `ackType=1`、`ackType=2` ACK；
- 其他条件相同。

**期望**

- 两次都成功；
- 服务端行为一致（当前不区分）。

### ACK-004 重复 ACK `[现状]`

**步骤**

1. ACK `upToSeq=5`；
2. 再 ACK `upToSeq=5`；
3. 查 `op=3` 数量。

**期望**

- `delivered_seq` 仍 5；
- `op=3` 不新增（dedupe key 相同）。

---

## 4. 边界用例

### ACK-010 upToSeq=0/负数 `[现状]`

**步骤**

- 发 `upToSeq=0`、`upToSeq=-1`。

**期望**

- 网关过滤，不调用 PushService；
- 无回执、无 `op=3`；
- 连接保持。

### ACK-011 upToSeq > max_seq `[现状]`

**前置**：`max_seq=5`。

**步骤**

- ACK `upToSeq=10`。

**期望**

- `delivered_seq=5`（截断）；
- `op=3` 的 seq=5；
- 不报错；ACK 无回传，客户端不依赖结果（cursor 独立）。

### ACK-012 max_seq=0 `[现状]`

**前置**：会话存在但无消息。

**步骤**

- ACK `upToSeq=5`。

**期望**

- `delivered_seq=0` 或 no-op；
- 不产生 `op=3`。

### ACK-013 非法 conversationID `[现状]`

**步骤**

- ACK `conversationID="abc"`、`"1_2_3"`、`"_"`。

**期望**

- PushService 返回 `INVALID_ARGUMENT`；
- 不写；
- 网关按现状断连（客户端修正后重连）。

### ACK-014 非参与者 `[现状]`

**步骤**

- 用户 C ACK A、B 的会话 ID。

**期望**

- no-op 成功；
- 不写 `im_conversation_receipts`；
- 网关不断连。

### ACK-015 会话已删除 `[现状]`

**前置**：删除 `im_conversations` 中该会话行。

**步骤**

- ACK `upToSeq=10`。

**期望**

- no-op 成功；
- 不写回执、不新增 `op=3`；
- 心跳仍可用（连接保持）。

> 该用例已由 `scripts/smoke/seq_test.py` 覆盖。

### ACK-016 群 ID 走 106 `[现状]`

**步骤**

- 用群 ID（非 `digits_digits`）作为 `conversationID`。

**期望**

- 现状：`conversationID` 不含 `_`，按非法处理，PushService 返回 `INVALID_ARGUMENT`，网关断连；
- 不写单聊回执；
- 客户端不应把群 ACK 走 `type=106`（群 ACK 走 `type=250`）。

### ACK-017 一帧重复会话 `[现状]`

**步骤**

- 同一帧带 `[{conv,5},{conv,10}]`。

**期望**

- 最终 `delivered_seq=10`；
- `op=3` 按 5 和 10 各一条（或目标优化后只留 10）。

### ACK-018 100 个会话一帧 `[现状]`

**前置**：用户有 100 个会话。

**步骤**

- 一帧 ACK 100 个会话。

**期望**

- 全部处理；
- 无超时/断连；
- 记录耗时。

### ACK-019 超大 upToSeq `[现状]`

**步骤**

- `upToSeq=INT64_MAX`。

**期望**

- 截断到 `max_seq`；
- 无溢出、无负数。

### ACK-020 空 conversations `[现状]`

**步骤**

- 发 `AckReq{ackType=1, conversations=[]}`。

**期望**

- 网关忽略；
- 不断连、不报错。

---

## 5. 乱序与提前

### ACK-030 100 后 80 `[现状]`

**步骤**

1. ACK 100；
2. ACK 80。

**期望**

- `delivered_seq` 100；
- 不回退；
- G2 优化后不再新增 `op=3(80)`（由 ACK-033 断言）。


### ACK-031 80 后 100 `[现状]`

**期望**

- 最终 100；
- 两条 `op=3`。

### ACK-032 并发 ACK `[现状]`

**步骤**

- 两个连接/两个 PushService 实例同时 ACK 100/80。

**期望**

- 最终 100；
- `GREATEST` 生效；
- 无死锁。

### ACK-033 低水位不新增 op=3 `[已实现]`


**前置**：`delivered_seq=100`。

**步骤**

- ACK 80。

**期望**

- `op=3` 数量不变；
- 直接返回成功。

### ACK-034 ACK 提前 `[现状]`

**前置**

- 消息 91–100 已落库，但客户端未处理；
- `max_seq=100`。

**步骤**

1. ACK 100；
2. 重新登录，带 `cursor=90`。

**期望**

- `delivered_seq=100`；
- 登录同步仍返回 91–100（Mongo 查询不按 status 过滤）；
- 客户端若把 cursor 推 100，则不会拉取（客户端契约问题）。

### ACK-035 ACK 时消息未提交 `[现状]`

**前置**

- 消息 99/100 还在 outbox stage=0；
- `max_seq=98`。

**步骤**

- ACK 100。

**期望**

- 截断到 98；
- 不报错；
- 99/100 后续提交后不被覆盖；
- 客户端需再次 ACK。

---

## 6. 投递与离线

### ACK-040 ACK 后 op=1 完成 `[现状]`

**前置**：在线消息已推，等待 ACK。

**步骤**

- ACK 水位覆盖该 seq；
- 等待 outbox worker。

**期望**

- 对应 `op=1` 任务 `state=2`；
- 不再重复推送。

### ACK-041 Mongo status=2 `[现状]`

**步骤**

- ACK 后等待 `op=3`。

**期望**

- `recvID=owner`、`convID`、`seq<=upToSeq` 的消息 `status=2`；
- 范围外不变。

### ACK-042 未 ACK 离线消息 `[现状]`

**步骤**

- 不 ACK，重新登录。

**期望**

- 209/200 照常返回；
- ACK 不是离线拉取的前置条件。

### ACK-043 ACK 乱序不影响 cursor `[现状]`

**步骤**

- 乱序 ACK 后重新登录，带相同 cursor。

**期望**

- 209 ranges 与未乱序时一致；
- ACK 不改 cursor 逻辑。

### ACK-044 重连重复 ACK `[现状]`

**步骤**

- 断线重连后重复 ACK。

**期望**

- 水位不变；
- `op=3` 不重复（同水位）。

---

## 7. 失败用例

### ACK-050 PushService 不可用 `[现状]`

**步骤**

- 停 PushService；
- 发 ACK。

**期望**

- 网关 `onDisconnect`；
- 客户端重连后可重发；
- 无半写。

### ACK-051 MySQL 不可用 `[现状]`

**步骤**

- 停 MySQL；
- 发 ACK。

**期望**

- ACK 失败；
- 不产生回执/任务；
- 恢复后可重试。

### ACK-052 op=3 失败 `[现状]`

**步骤**

- 注入 Mongo 更新失败（或停 Mongo）。

**期望**

- `op=3` 保持待执行；
- 退避重试；
- 恢复后 `status=2`。

### ACK-053 ACK RPC 超时 `[现状]`

**步骤**

- 让 PushService 延迟 >8 秒。

**期望**

- 网关断连；
- 客户端重试；
- 服务端若已提交，重试幂等。

### ACK-054 Redis 不可用 `[现状]`

**步骤**

- 停 Redis；
- 发 ACK。

**期望**

- ACK 仍成功（只写 MySQL）；
- 在线推送/路由可能受影响，但不影响水位。

---

## 8. 性能与容量

### ACK-060 100 会话一帧

**指标**

- 网关处理时间 <100ms（本地）：网关是 fire-and-forget，只等 RPC 派发，客户端侧无法单独测量；
- PushService 事务逐个会话，端到端总耗时记录（多次实测 424ms～1.7s，见需求文档 F1 说明）。




### ACK-061 1000 条逐步 ACK

**步骤**

- 逐步 ACK 1..1000。

**期望**

- `op=3` 行数 ≤ 1000（目标优化后 ≤ 推进次数）；
- 无锁等待超时。

### ACK-062 回执行数

**步骤**

- 同一会话发 10 万条并 ACK。

**期望**

- `im_conversation_receipts` 仍 1 行；
- 不随消息数增长。

---

## 9. 回归

### ACK-070 209 登录同步

- ACK 前后分别登录；
- 209 ranges 不受 ACK 影响（cursor 相同）。

### ACK-071 244 拉历史

- ACK 前后 `Pull` 返回一致（按 afterSeq）。

### ACK-072 104/200 去重

- 同一条消息经 104 和 200 各来一次；
- 客户端按 `serverMsgID` 去重，ACK 只推进一次。

---

## 10. 自动化建议

已实现：`scripts/smoke/ack_test.py`（输出 PASS/SKIP/WARN/FAIL 统计，`--json` 落盘）。

1. 复用 `group_test.register` / `login` / `ws_test.WS`；
2. 用 `build_thread` 准备会话水位：默认直接构造 `im_conversations` + MongoDB 文档（不依赖 Kafka，快且可重复），
   `--e2e` 时改为真实 A→B 发送；
3. 发 `type=106` ACK 帧；
4. SQL / Mongo 断言（`delivered_seq`、`op=3` 行数、`op=1` state、Mongo `status=2`、Pull 结果、连接存活）；
5. 故障注入：`--faults` 用 `pkill` 停 PushService 再重启（ACK-050）；
6. 输出 JSON 结果（可选 JUnit 由外层 CI 转换）。

```bash
bash scripts/smoke/start_ack.sh           # backbon/auth/msg/push/gw-1/gw-2 + 2s 清理间隔
python3 scripts/smoke/ack_test.py --json /tmp/ack-results.json
python3 scripts/smoke/ack_test.py --faults --case ACK-050   # 故障注入
python3 scripts/smoke/ack_test.py --e2e --case ACK-001 --case ACK-041  # 真实发送链路
```

最近一次执行结果：`{'PASS': 37, 'SKIP': 6, 'WARN': 1, 'FAIL': 0}`（65.3s，明细见 §12.1）。

### 10.1 断言语义

- `delivered_seq` 精确值；
- `op=3` 行数；
- `op=1` state；
- Mongo `status=2` 数量；
- 连接存活（ping/pong）。

### 10.2 与现有脚本关系

- `seq_test.py` 已覆盖 ACK-001/004/015；
- 新增脚本覆盖乱序、越界、非参与者、失败注入；
- `group_test.py` 只保证群聊回归，不覆盖单聊 ACK。

---

## 10.1 客户端主动补洞（客户端用例）

> 本期假设客户端可信；这些用例用于验收客户端契约，不作为服务端强制。
> 服务端不回传 ACK 结果，客户端必须自己发现缺口并 Pull。

### CLIENT-001 乱序到达 `[客户端]`

**步骤**

- 客户端依次收到 seq 1、2、5。

**期望**

- `contiguousSeq=2`，5 进 buffer；
- 立即 `Pull(afterSeq=2, limit=100)`；
- 返回 3、4 后 flush 5，`contiguousSeq=5`；
- ACK 5。

### CLIENT-002 209 hasGap `[客户端]`

**前置**

- 登录同步 209 中 `range.hasGap=true`。

**期望**

- 对该会话 `Pull(afterSeq=本地连续水位)`；
- 直到 `hasMore=false`，再推进 cursor。

### CLIENT-003 ACK 被截断但不回传 `[客户端]`

**前置**

- 服务端 `max_seq=103`；
- 客户端 ACK 110。

**期望**

- 服务端静默记到 103，不回响应；
- 客户端仍以本地 `contiguousSeq=103` 为准；
- 104–110 后续提交后，客户端通过 244/209 主动拉取。

### CLIENT-004 ACK 提前，cursor 未提前 `[客户端]`

**前置**

- 104–110 已存在，客户端只处理到 103，却 ACK 110；
- 服务端回执到 110、Mongo `status=2`；
- 客户端本地 cursor 仍是 103。

**期望**

- 重新登录/主动 Pull 从 103 开始，仍能拿到 104–110；
- 不因 ACK 提前而跳过。

### CLIENT-005 永久空洞 `[客户端]`

**前置**

- 104–105 因删除/清理永久缺失；
- Pull 返回 106 且 `hasMore=false`。

**期望**

- 客户端不能直接跳到 106，除非服务端明确 `minSeq/deletedSeq` 表明 104–105 永久不可用；
- 否则退避重拉，避免把“尚未提交”误判为删除。


### SERVER-001 ACK 提前后仍可 Pull `[现状]`

**前置**

- 消息 104–110 已存在；
- 客户端只处理到 103，却 ACK 110；
- 服务端 `delivered_seq=110`，Mongo 104–110 `status=2`。

**步骤**

- 客户端以 `afterSeq=103` 调用 244。

**期望**

- 仍返回 104–110（同步/Pull 不按 `status` 过滤）；
- 客户端补齐后 cursor=110；
- 不因 ACK 提前而丢失。

### SERVER-002 临时缺口 `[现状]`

**前置**

- 106 已落库，104/105 未落库；
- 客户端 cursor=103。

**步骤**

- Pull(afterSeq=103) 返回 106；
- 客户端 buffer，不推进 cursor；
- 退避重试；
- 104/105 落库后再次 Pull。

**期望**

- 第二次 Pull 返回 104、105、106；
- 客户端补齐，cursor=106；
- 106 延迟上屏，不丢。

---

## 11. Outbox 清理（第二层策略，规划）

> 清理线程已实现（需求文档 11.1）：`scripts/smoke/start_ack.sh` 会把
> `OUTBOX_CLEANUP_INTERVAL_MS` 调成 2s，便于在测试里观察保留期删除。


### OUTBOX-001 op=1 超过 7 天删除 `[已实现]`


**前置**

- `im_outbox` 中有一条 `operation=1,state=2`，`updated_at = now-8d`；
- 另一条 `operation=1,state=0`。

**步骤**

- 启动/触发清理；
- 查询两行。

**期望**

- `state=2` 行被删除；
- `state=0` 行保留。

### OUTBOX-002 op=3 超过 1 天删除 `[已实现]`


**前置**

- `operation=3,state=2,updated_at=now-2d`；
- `operation=1,state=2,updated_at=now-2d`（未到 7 天）。

**期望**

- op=3 行删除；
- op=1 行保留。

### OUTBOX-003 op=2 保留 7 天 `[已实现]`


- `operation=2,state=2,updated_at=now-8d` → 删除；
- `updated_at=now-6d` → 保留。

### OUTBOX-004 批大小与多轮 `[已实现]`


**前置**：1500 条可删除行。

**步骤**：设置 `OUTBOX_CLEANUP_BATCH=1000`，触发一轮。

**期望**

- 本轮最多删 1000 条（或按 maxBatches 配置）；
- 下一轮继续删剩余；
- 不出现长事务/锁等待超时。

### OUTBOX-005 多实例并发 `[已实现，单实例环境未验证]`


**前置**：两个 MsgService 实例同时清理。

**期望**

- 不报错；
- 行只被删一次；
- 未完成任务不被删。

### OUTBOX-006 幂等窗口 `[已实现]`


**前置**：`op=1,state=2,updated_at=now-6d`。

**步骤**

- 用同一 `clientMsgID` 重发。

**期望**

- 命中旧行，不新增消息/seq。

**对照**：`updated_at=now-8d` 时旧行已被删，重发会当新消息（文档明确的窗口边界）。

---

## 12. 退出标准

- 需求文档 `docs/design/messaging.md` 中 A–H 验收项全部通过；
- D6–D10 客户端自检/主动 Pull 用例通过；
- 目标项 G2 若本期实现，ACK-033 的“低水位不新增 op=3”断言通过；
- 现有 `seq_test.py`、`group_test.py`、`all_test.py` 回归通过；
- 无新增断连/半写/回退缺陷。

### 12.1 最近一次执行结果（2026-11-24）

```
scripts/smoke/ack_test.py：44 个用例 / 65.3s
{'PASS': 37, 'SKIP': 6, 'WARN': 1, 'FAIL': 0}
指标：ACK-018.elapsed_ms=1473, ACK-061.op3_rows=1000, outbox.cleanup_active=true
明细：docs/testing/results/ack-2026-11-24.json

scripts/smoke/ack_test.py --faults --case ACK-050：PASS
scripts/smoke/seq_test.py、group_test.py、all_test.py：全部 PASSED
```

- SKIP：ACK-050（默认不注入）、ACK-051、ACK-053、ACK-054、ACK-072、OUTBOX-005。
- WARN：ACK-060（端到端 1473ms，多次运行区间 424ms～1.7s，指标口径见需求文档 F1）。
