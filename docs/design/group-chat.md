# 群聊设计（v3）

> 状态：已实现（v3 主链路）  
> 权威性：本文是「群聊 v3 架构、设计决策、协议与数据模型」的唯一设计来源  
> 最后核对：2026-11-26 @ `03ed7c2` + 工作区改造  
> 适用代码：src/services/group_service/、src/services/push_service/group_fanout_handler.cpp、src/storage/mysqlhandler_group.cpp、proto/groupchat.proto

> 范围：群聊 v3（Mongo 单副本 + `im_group_stream` + outbox `op=4` + Kafka 扇出 + `245/246/210/251`）。
> 单聊见 [messaging.md](messaging.md)，测试见 [../testing/group-chat.md](../testing/group-chat.md)。

---

## 0. 一页纸结论

群聊 = **一条写流 + 多个读者**：消息单副本存 Mongo，每群一个稠密 seq，在线推送、离线/漏推 Pull。

| 环节 | v3 结论 |
|---|---|
| 消息存储 | Mongo `IM-System.msg`，一消息一文档；`convID=groupID`、`isGroupMsg=true`、`recvID=0`、`sessionType=2` |
| 消息格式 | 全链路统一 `sdkws.MsgData` |
| seq | MySQL `im_group_stream.next_seq` 单行分配，每群 1,2,3… 严格 +1 |
| 发送 | MySQL 单事务：成员行锁 → seq → `staging(正文) + outbox(op=4, payload=引用)`；提交即返回 |
| 落库/发布 | worker：写 Mongo(`committed=false`) → `PublishMessage` + Kafka `group_topic` |
| 扇出 | PushService：读 Mongo/成员 → `PushBatchBestEffort(251)` |
| 推送语义 | 尽力而为（可能重复/丢失/乱序）；不丢靠 Mongo + Pull |
| 拉取 | 新帧 245：Mongo 按 `(convID, seq)` 范围读，`minSeq=joinSeq` |
| 登录同步 | 单聊 209 后，群 251（小群内联）+ **210 GroupSyncSummary** |
| ACK/未读 | **无 ACK**；未读客户端本地 `lastReadSeq`，服务端零状态 |
| 兼容 | **直接切新协议**；250 只保留成员/元数据命令 |

---

## 1. 与单聊对照

| 维度 | 单聊（已实现） | 群聊（v3） |
|---|---|---|
| 消息存储 | Mongo，每收件人一份 | Mongo，**每群一份** |
| 消息格式 | MsgData | MsgData（`isGroupMsg=true`、`recvID=0`） |
| seq | `im_conversation_seq` | `im_group_stream.next_seq` |
| 发送事务 | `outbox(op=1)`，worker 写 Mongo + 发布 | `outbox(op=4)`，worker 写 Mongo + 发布；**无每成员工作** |
| 发布 | Kafka `msg_topic` key=convID | Kafka `group_topic` key=groupID |
| 在线扇出 | `PushFrame(104)` | `PushBatchBestEffort(251)` |
| ACK | 106 水位 + receipts + op=3 | **不存在**（群不做 ACK） |
| 离线恢复 | 209/200/244 | 210/251/245 |
| 幂等 | outbox dedupe 7 天 | 同 |
| 未读 | 服务端计数 | 客户端本地 |

---

## 2. 架构

```
Client A ──246(Send)──► Gateway ──gRPC──► GroupService
                                          │ MySQL tx: 成员行锁 → seq → staging + outbox(4, 引用)
                                          ▼
                                   GroupOutboxWorker
                                   1) SaveMsgToMongo(committed=false)
                                   2) PublishMessage + Kafka produce
                                          ▼
                                   Mongo IM-System.msg
                                          ▲
                                   Kafka group_topic (key=groupID)
                                          │
                                          ▼
                                   PushService GroupFanout
                                          │ 读消息/成员/路由
                                          ▼
                                   PushBatchBestEffort(251)
                                          │
                              Gateway-1 / Gateway-2 ──► 251
Client B ──245(Pull)──► Gateway ──gRPC──► GroupService ──► Mongo 范围读
Client B ──91(login)─► Gateway ── 209(单聊) + 251(群内联) + 210(群同步完成)
```

### 2.1 角色

| 角色 | 职责 |
|---|---|
| Gateway | 鉴权、覆盖 actor、帧校验、登录同步、写队列背压 |
| GroupService | 成员/元数据、发送事务、245、210 查询、outbox worker、刷新通知 |
| GroupOutboxWorker | 领 op=4、写 Mongo、发布 Kafka、退避 |
| PushService GroupFanout | 消费 group_topic、读消息/成员、按网关批量推 251 |
| GatewayRouter | account→实例；群扇出 best-effort，不广播兜底 |
| Mongo / MySQL / Redis / Kafka | 消息正文 / 元数据与 seq+outbox / 缓存与路由 / 事件通道 |

### 2.2 帧

| type | 方向 | 载体 | 说明 |
|---|---|---|---|
| 91 | C→S | `SdkWSReq` | 登录；`cursors` 带每群 cursor |
| 209 | S→C | `SyncSummary` | 单聊同步结束（不变） |
| **210** | S→C | `groupchat.GroupSyncSummary` | **群同步结束** |
| **245** | C→S/S→C | `groupchat.GetGroupChatHistory*` | 群历史 |
| **246** | C→S/S→C | `groupchat.SendGroupMessage*` | 群发送 |
| 250 | C→S/S→C | `groupchat.Command/Reply` | **只保留成员/元数据/申请命令** |
| 251 | S→C | `groupchat.Notice` | 新消息/同步内联/刷新/解散 |
| 101/104/106 | — | 单聊 | 不变；群不复用 |

> 群没有 ACK 帧；`107` 也不存在（见 §11）。

---

## 3. 不变量

| 编号 | 不变量 |
|---|---|
| G1 | 每群 seq 从 1 严格 +1、无空洞（stream 单行分配；回滚回滚计数器） |
| G2 | 一条群消息在 Mongo 只有一份，不按成员复制 |
| G3 | 发送事务提交=已接受；Mongo/发布由 worker 最终完成 |
| G4 | 可见 = Mongo 文档存在且 `committed != false`；推送不是可见性前提 |
| G5 | 推送尽力而为；客户端按 serverMsgID 去重、按 seq 排序 |
| G6 | 可恢复：`active=1` 且 `seq>joinSeq` 可 245 拿到全部已发布消息 |
| G7 | `joinSeq` = 入群时 next_seq；重入重置 |
| G8 | `epoch` 权威在 stream，成员变更 +1 |
| G9 | 幂等：同 (group,sender,clientID) 在 outbox 保留期内至多一条 |
| G10 | 发送只锁成员行+stream 行；245/同步/列表不加群锁 |
| G11 | 推送不保证顺序；存储/Pull 按 seq 有序 |
| G12 | 无 ACK；客户端本地状态不参与服务端可见性/投递 |
| G13 | `published_seq` = 已发布（含 tombstone）的最大 seq；`Group.lastSeq`/210/245 上界用它，不用 `next_seq` |
| G14 | outbox 放弃 Mongo 写必须写 tombstone；客户端跳过 `isDeleted` 但推进 cursor；永久空洞可表达 |
| G15 | 客户端只按实际拉到的连续消息推进 cursor；前台每 30s 245 对账；不能用 lastSeq 推 cursor |

---

## 4. 数据模型

完整 DDL/索引见 [协议与数据模型（附录 B）](#附录-b协议与数据模型v3)。

### 4.1 MySQL

```sql
im_groups(id,name,owner,member_count,dismissed,created_time,updated_at)
im_group_stream(group_id PK,next_seq,published_seq,last_msg_time,epoch,dismissed,updated_at)
im_group_members(group_id,account,active,epoch,join_seq,joined_time,left_time,
                 PRIMARY KEY(group_id,account), KEY account_active(account,active,group_id))
im_group_numbers / im_group_applications / im_group_requests   -- 沿用
im_group_send_staging(server_msg_id PK,group_id,seq,payload,created_at,updated_at)  -- 正文暂存
im_outbox(operation=4, dedupe_key='g:{gid}:{sender}:{clientID}', payload='{"serverMsgID":...}', ...)
```

不再有：`im_group_messages`、`im_groups.last_seq`、成员 `ack_seq/read_seq`（无 ACK）。

- `next_seq` = 已分配的最大 seq；`published_seq` = 已发布（Mongo `committed=true`，含 tombstone）的最大 seq；
  `Group.lastSeq`/同步/245 上界都用 `published_seq`，不是 `next_seq`；
- `im_group_send_staging` 与 seq/outbox **同一 MySQL 事务**写入；worker 写完 Mongo 后删除，
  避免正文在 outbox 里保留 7 天；staging 是 MySQL 本地表，不引入跨存储一致性问题。

### 4.2 Mongo

```js
// IM-System.msg
{ serverMsgID, sendID, recvID:0, convID:"<groupID>", clientMsgID,
  senderPlatformID, senderNickname, senderFaceURL,
  sessionType:2, msgFrom, contentType, content:BinData,
  seq, sendTime, status, isGroupMsg:true, isDeleted:false, ext,
  committed:false }
```

索引沿用 `{convID:1,seq:1}`、唯一 `{serverMsgID:1}`；群查询加 `isGroupMsg:true`。

**空洞 tombstone**：outbox 放弃 Mongo 写时，写一条 `isDeleted=true`、`committed=true`、content 为空的
`MsgData` 文档；客户端看到 `isDeleted` 跳过正文但推进 cursor；`245` 用 `deletedSeq` 提示本页最大空洞。

### 4.3 Redis / Kafka

| 资源 | 约定 |
|---|---|
| `grp:members:{gid}` | HASH account→joinSeq，TTL 600s；成员变更 DEL |
| `grp:rate:m:{gid}:{uid}` / `grp:rate:g:{gid}` | 1s 限流 |
| `gw:user:{account}` | 网关路由（已有） |
| Kafka `group_topic` | key=groupID，payload=`traceparent` + `serverMsgID`（竖线分隔），消费组 `push-group-fanout` |

---

## 5. 发送

### 5.1 事务

```sql
START TRANSACTION; -- READ COMMITTED

-- 0) 幂等快路径：命中则从 staging/Mongo 取 MsgData 返回
SELECT id,server_msg_id,seq FROM im_outbox WHERE operation=4 AND dedupe_key=?;
-- 然后 SELECT payload FROM im_group_send_staging WHERE server_msg_id=?
--      或 Mongo 按 server_msg_id 读（staging 已删时）

-- 1) 成员行锁
SELECT active FROM im_group_members
 WHERE group_id=? AND account=? FOR UPDATE;
-- 无行/active=0 -> NOT_MEMBER

-- 2) seq + 准入
UPDATE im_group_stream
   SET next_seq=LAST_INSERT_ID(next_seq+1), last_msg_time=?, updated_at=?
 WHERE group_id=? AND dismissed=0;
-- affected=0 -> GROUP_NOT_FOUND / GROUP_DISMISSED
SELECT LAST_INSERT_ID();

-- 3) 正文暂存 + outbox 引用（同一事务）
INSERT INTO im_group_send_staging(server_msg_id,group_id,seq,payload,created_at,updated_at)
VALUES (?,?,?,?,?,?);
INSERT INTO im_outbox
 (operation,dedupe_key,actor_account,peer_account,conversation_id,
  server_msg_id,seq,payload,stage,created_at,updated_at)
VALUES (4,?,?,0,?,?,?,?,0,?,?);  -- payload 只存 {"serverMsgID":"..."}

COMMIT;
```

- 返回 `MsgData{seq,serverMsgID,sendTime,...}`；**不等 Mongo**；
- 并发同 clientID：outbox 唯一键冲突 → ROLLBACK（seq 回滚，无空洞）→ 从 staging/Mongo 读获胜者 → 返回同一消息；
  内容不同 → `IDEMPOTENCY_CONFLICT`；
- 幂等窗口 = outbox 7 天（与单聊一致）；
- staging 与 outbox 同事务，正文不会出现“有 outbox 无正文”；worker 成功后删除 staging。

### 5.2 校验/限流

| 项 | 规则 | 错误码 |
|---|---|---|
| groupID | 纯数字 1..20、非 0 | `INVALID_GROUP` |
| 成员 | `active=1` | `NOT_MEMBER` |
| 群状态 | `dismissed=0` | `GROUP_DISMISSED`/`GROUP_NOT_FOUND` |
| content | 非空、≤64KiB | `EMPTY_MESSAGE`/`MESSAGE_TOO_LARGE` |
| contentType | 本期 101 | `INVALID_PARAM` |
| clientID | 非空、≤64、可打印 ASCII | `INVALID_REQUEST_ID` |
| 成员限流 | 5/s/群 | `RATE_LIMITED` |
| 群限流 | 50/s | `RATE_LIMITED` |

限流在事务前；拒绝不消耗 seq；Redis 挂时放行，群级上限兜底。网关另设 WS 帧上限（1MiB）。

---

## 6. 落库/发布（outbox op=4）

```
state=0 --claim(lease 45s)--> state=1
  stage=0: 读 staging -> SaveMsgToMongo(committed=false) -> stage=1 -> 删 staging
  stage=1: PublishMessage(committed=true) -> published_seq=GREATEST(published_seq,seq) -> Kafka 确认
state=1 --成功--> state=2
stage=0 --超过 GROUP_PUBLISH_MAX_ATTEMPTS--> 写 tombstone(committed=true,isDeleted=true) -> published_seq -> state=3
stage=1 --超过 GROUP_PUBLISH_MAX_ATTEMPTS--> state=3（消息已 committed，可 Pull）
```

- Kafka 失败：state=0、available_at+5s，stage=1，只重试发布；
- Mongo 失败：stage=0，重试整段；
- Mongo 超过最大尝试：先查 Mongo——已存在未发布 → `PublishMessage` + `published_seq` + state=2；
  不存在 → **写 tombstone** + `published_seq` + state=3；已存在已发布 → `published_seq` + state=3；
  tombstone 写失败则继续重试（绝不能留不可见空洞）；超过最大尝试后**不再尝试写正文**，只重试 tombstone；
- `published_seq` 在 PublishMessage/tombstone 成功后 `GREATEST` 推进，供 Group.lastSeq/同步/245 使用；
- staging 清理：stage=1 后删除；孤儿按 `created_at` 保留期清理；
- 不等待任何成员确认（无 ACK）；发布成功即 state=2；
- `ClaimOutboxTask` 按 operation 过滤：MsgService `{1,2,3}`，GroupService `{4}`；
- 事件 key=groupID，payload=`traceparent|serverMsgID`；消息体不进 Kafka；
- 消费端按 serverMsgID 读 Mongo（可加 `msg:{id}` 缓存）。

---

## 7. 扇出

```
consume(group_topic, key=groupID, payload=traceparent|serverMsgID)
1. 读 MsgData（Redis/Mongo）；不存在 -> 返回
2. 读 stream：dismissed/epoch/published_seq；解散 -> 返回
3. 成员：HGETALL grp:members:{gid}；miss 回源 SQL(active=1)
4. 过滤 joinSeq<seq 且 account!=sender
5. activeMembers<=200（排除 sender 前）且字节<=256KiB -> Notice{messages:[MsgData]}；否则 Notice{lastSeq,epoch}
6. 500 分片 PushBatchBestEffort(251)
7. 汇总 Delivered/NotOnline/Unknown，记指标后完成；不阻塞分区、不无限重试
```

**为什么 best-effort**：现有 `PushBatch` 对无路由/实例否认的账号会广播给所有网关；
2000 人群 1900 离线时每条消息 O(离线×实例) 放大。群推送尽力而为，Pull 兜底，因此
无路由/实例否认 → NotOnline，不广播；实例不可达 → Unknown，不重试。

| active 成员 | 251 | 客户端 |
|---|---|---|
| ≤200 | 带 1 条 MsgData（同步≤20） | 直接渲染 |
| 201–2000 | 只带 lastSeq/epoch | 245 拉取 |
| >2000 | 拒绝入群 | — |

阈值按 **active 成员数（排除 sender 前）** 判断，`recipients` 只用于实际发送；
这样 201 人群即使发送者在群内也只推 hint，与 `Group.large`/`member_count` 口径一致。

成本：MySQL 写 O(1)（stream+outbox）、Mongo 写 O(1)、扇出读 O(N)、RPC O(G×N/500)、帧 O(在线)、存储 O(消息)。

---

## 8. 拉取（245）

```proto
message GetGroupChatHistoryReq {
  uint64 ownerUserID=1; string groupID=2;
  int64 afterSeq=3;   // 向后：seq > afterSeq（增量/补洞）；0=joinSeq
  int32 limit=4;      // 1..100，默认 20
  int64 snapshot=5;   // 上界，0=请求时 published_seq
  int64 beforeSeq=6;  // 向前：seq < beforeSeq（最新/翻旧）；与 afterSeq 互斥
}
message GetGroupChatHistoryResp {
  repeated sdkws.MsgData messages=1; // 始终升序
  bool hasMore=2; int64 nextSeq=3;
  int64 minSeq=4; int64 lastSeq=5; uint64 epoch=6;
  int64 deletedSeq=7; // 本页最大永久空洞 seq；0=无；客户端以 MsgData.isDeleted 为准
}
```

1. 校验群/成员：不存在/解散/非成员 → 错误；`afterSeq` 与 `beforeSeq` 同时 >0 → `INVALID_PARAM`；
2. `lastSeq=published_seq`（已发布最大 seq，不是 next_seq）；
   `to=min(snapshot>0?snapshot:lastSeq, lastSeq)`，`minSeq=joinSeq`；
3. **向后模式**（`afterSeq>0`）：`from=max(afterSeq,joinSeq)`，查 `seq>from && seq<=to`，升序 `limit+1`；
   `hasMore=rows>limit`；`nextSeq=最后一条 seq`（空页 `nextSeq=from`）；
4. **向前模式**（`beforeSeq>0`）：`upper=min(beforeSeq,to+1)`，查 `seq<upper && seq>joinSeq && seq<=to`，
   按 `seq` 倒序取 `limit+1` 再反转为升序；`hasMore=rows>limit`；
   `nextSeq=最早一条 seq`（空页 `nextSeq=joinSeq`）；
5. **两者都为 0**：向后模式 `from=joinSeq`（从入群起点拉），**不是最新**；
   要“最新 N 条”必须用 `beforeSeq=lastSeq+1`；
6. 所有模式都只返回 `committed != false`；tombstone（`isDeleted=true`）也返回，客户端跳过正文但**推进 cursor**；
7. `deletedSeq=本页最大 isDeleted seq`（无则 0）；它只是提示，客户端必须以 `isDeleted` 为准逐条跳过。

> 本期**不支持撤回/删除**。永久空洞只可能来自 outbox 放弃 Mongo 写 → tombstone；
> 未来做撤回/删除时，复用同一 tombstone + `deletedSeq` 语义，并保证 `minSeq` 能表达永久下界。

典型用法：首次加载/大群 `needsPull` 用“最新 N”；补洞用 `afterSeq=cursor`；
向上翻历史用 `beforeSeq=已加载最早 seq`。

**客户端契约（关键）**：

- 只推进“实际拉到的连续前缀”到 cursor；**不能用 `lastSeq` 直接推进 cursor**；
- 收到 `isDeleted=true` 的消息：跳过正文，但该 seq 视为已处理，可推进 cursor；
- 前台打开某个群时，每 **30s** 做一次 `245{afterSeq=cursor,limit=20}` 对账（推送可能丢）；
- 发现 `seq > cursor+1` 缺口：立即 245；补齐连续前缀后再推进；
- `lastSeq` 只表示“服务端已发布到的位置”，不代表客户端已有这些消息。

---

## 9. 登录同步（210）

1. 单聊同步照旧（200/209）；
2. 群同步（网关直接读 MySQL/Mongo）：
   - active 群按 `last_msg_time DESC`，上限 50；
   - 读 `published_seq`（作为 `lastSeq`）/`joinSeq`/`epoch`；`cursor>=lastSeq` 跳过；
   - 小群且待补≤20：Mongo 查已发布消息（含 tombstone），发一帧 `251{messages}`；
   - 大群/超限：不发 251，只写 `GroupSyncRange{needsPull=true}`；
   - 群消息总数≤200，超出转 needsPull；
3. 最后发 **210**（即使空也发，作为群同步结束信号）。

`GroupSyncRange.fromSeq/toSeq`：`floor=max(cursor,joinSeq)`，`fromSeq=floor+1`；
`toSeq` = 实际内联到的最后一条 seq；**没有内联时为 `fromSeq-1`（空覆盖）**，不是 `lastSeq`。
`needsPull = (toSeq < lastSeq)`；`hasGap = fromSeq > cursor+1`（即 `cursor<joinSeq`）。
同步内联受 `GROUP_INLINE_MAX_BYTES=256KiB` 限制：按字节截断条数，0 条能放下则转 `needsPull`。

```proto
message GroupSyncRange {
  string groupID=1; int64 fromSeq=2; int64 toSeq=3; int64 lastSeq=4;
  int64 joinSeq=5; uint64 epoch=6; bool hasGap=7; bool needsPull=8;
}
message GroupSyncSummary {
  repeated GroupSyncRange groups=1; uint32 messagesPushed=2; bool hasMore=3;
}
```

客户端：251 先渲染；210 表示群同步结束；`needsPull/hasGap` 或 `lastSeq>cursor` → 245；
补齐连续前缀后再推进 cursor；**不能用 lastSeq 直接推进**；未读本地算（§11）。

**首次加载/大群 needsPull**：用 `beforeSeq=lastSeq+1` 取最新 N 条；
之后用 `afterSeq=cursor` 增量/补洞；向上翻历史用 `beforeSeq=已加载最早 seq`。
前台当前群每 30s 再做一次 `245(after=cursor)` 对账，弥补 best-effort 推送丢失。

降级：>50 群 210 `hasMore`；单群失败 210 `PARTIAL`；解散群不入 ranges；无 cursor 从 joinSeq 起。

---

## 10. 成员

| 操作 | 关键动作 |
|---|---|
| Create | 插 groups/stream/群号/owner 成员（joinSeq=0,count=1） |
| Apply | 非成员、无 pending；插 applications |
| Review(accept) | 锁申请行→成员行（可能无）→stream；joinSeq=next_seq；epoch+1；count+1；active=1 |
| Review(reject) | 更新申请状态 |
| Leave | 非 owner；锁成员行→stream；active=0；epoch+1；count-1 |
| Kick | owner；同 Leave，目标成员 |
| Dismiss | owner；锁 stream；dismissed=1；epoch+1；关 pending |
| Rename | 更新 name；刷新通知 |

joinSeq/epoch/锁顺序/缓存失效同 v2：成员行（存在时）→stream；Dismiss 只锁 stream；
成员变更 DEL `grp:members:{gid}`；TTL 600s 兜底。

---

## 11. 无 ACK；未读客户端本地

**ACK 不存在**：服务端不维护“消息是否送达某成员客户端”的状态。理由：

- 群消息是单副本，没有每成员收件箱，服务端没有“送达”事实可记；
- 客户端收没收到、渲染没有，只有客户端知道；上报也只是自称，不能证明送达；
- 消息不丢靠 Mongo + Pull，不靠 ACK；
- 逐群 ACK 会引入额外写、重试、断连语义，收益为负。

**未读**：服务端零状态；客户端本地保存每群 `lastReadSeq`，
`unread = lastSeq - max(lastReadSeq, joinSeq)`。换设备/重装未读会丢失；本期单设备，可接受。

**本期不做**：`107`、`ack_seq`、`read_seq`、`unread`、任何服务端已读/送达状态。
若未来产品要“已读人数”，那是独立的 read-receipt 功能，不是送达 ACK，另行设计。

---

## 12. 失败与边界

| 故障 | 发送方 | 其他成员 | 恢复 |
|---|---|---|---|
| MySQL 不可用 | DB_ERROR，无 seq | 无法 Pull/成员变更 | 重试同 clientID |
| Mongo 不可用 | 发送成功，消息暂不可见 | 无 245 | worker 重试；恢复后可见 |
| Mongo 超最大尝试 | 发送成功 | 写 tombstone（`isDeleted=true`），客户端跳过 | 告警；tombstone 写失败继续重试，绝不留下不可见空洞 |
| Kafka 不可用 | 发送成功 | 无推送，Pull 可拉 | outbox 重试 |
| PushService 不可用 | 发送成功 | 无推送，Pull 可拉 | Kafka 位点 |
| GroupService 不可用 | DB_ERROR/超时 | 245/210 失败 | 重试 |
| 网关不可用 | 连接断开 | 其他网关正常 | 重连 210/245 |
| 网关 Unknown | — | 该批可能漏推 | 重连/切前台 Pull；扇出不重试 |
| Redis 不可用 | 限流放行 | 回源 SQL；无路由→Pull（不广播） | 自动 |
| 扇出崩溃 | — | 部分漏推/重复 | Kafka 重投或 Pull |
| 群解散/踢人 | 后续拒绝 | 251 通知；in-flight 按 epoch 忽略 | 产品策略 |

一致性：
- 顺序：seq 串行分配；发布可能乱序；客户端排序；
- 重复：outbox/Kafka/同步重叠；serverMsgID 去重；
- 缺口：发送后 worker 未完成时 Pull 暂时没有该 seq（`published_seq` 也还没推进）；buffer + 退避 245；
- `minSeq=joinSeq` 表示入群前不可见；`deletedSeq`/`isDeleted` 表示永久空洞；
- 客户端只能用“实际拉到的连续消息”推进 cursor，不能用 `lastSeq`；
- 本期无 ACK，可见性/恢复不以 ACK 为前提。

---

## 13. 直接切换与重置

**破坏性变更**：
- 250 只保留成员/元数据命令；Send/Ack/Read/Pull 返回 `UNSUPPORTED_OPERATION`；
- 客户端必须用 246/245/210；旧客户端不兼容；
- 删除 `im_group_messages`；`im_groups.last_seq`、成员 ack/read 不再使用；
- 不做双写/迁移/兼容开关。

**重置**：停服务 → 删除 group 表（含 `im_group_send_staging`）与 Mongo `isGroupMsg=true` mock 文档
（或 DROP 后由 `InitializeGroups` 重建）→ 重新生成 proto → 部署 GroupService/PushService/Gateway → 跑测试。

**文件改动**：
- proto：新 210/245/246；`Notice.messages` 改 `MsgData`；Group 去掉 unread/ack/read；
- gateway：246/245/210；250 只路由成员命令；旧 op 拒绝；
- group_service：Send/History/Sync + outbox；删除 Deliver；
- push_service：GroupPushHandler + 第二 consumer；
- gateway_router：PushBatchBestEffort；
- mongohandler：群历史/最新消息查询；
- mysqlhandler：新 schema/send tx/outbox/staging；`published_seq`、tombstone；删除 im_group_messages/ack/read；
- main：`RoleNeedsMongo("group")` 改为 true（群历史/outbox worker 需要 Mongo）；
  GroupService producer/outbox；PushService 第二 consumer；
  `kGroupMethods` 加 `SendGroupMessages/GetGroupChatHistory`；`GroupClient` 加对应方法。

---

## 14. 验收与开放问题

### 验收项（v3 已实现；以测试结果为准）
- [ ] A1 seq 1..N 连续；并发 1000 条。
- [ ] A2 Mongo 一条群消息一份文档。
- [ ] A3 发送后 outbox op=4；stage=1 后 Mongo 有文档；发布后 committed=true。
- [ ] A4 同 (group,sender,clientID) 7 天内重试返回同一 seq/serverMsgID。
- [ ] A5 245 只返回 committed=true、seq>joinSeq；tombstone 以 `isDeleted` 返回并带 `deletedSeq`。
- [ ] A6 staging 与 outbox 同事务；worker 写 Mongo 后删 staging；outbox payload 不存正文。
- [ ] A7 `published_seq` 在 Publish/tombstone 后推进；Group.lastSeq/210/245 用 `published_seq`。
- [ ] B1 ≤200 人群在线 P99<1s；B2 发送 P99<50ms；B3 大群 hint+245；B4 跨网关；B5 离线登录补齐。
- [ ] C1 209 后收到 210；C2 小群内联/大群 needsPull；C3 245 分页不丢不重；C4 上限/PARTIAL；
  C5 `needsPull=(toSeq<lastSeq)`，内联未覆盖 `published_seq` 时必须 Pull。
- [ ] D1 joinSeq/epoch；D2 踢人；D3 解散；D4 竞态；D5 2000 人/群号。
- [ ] E1 Mongo 故障；E2 Kafka/Push 故障；E3 Unknown 不阻塞；E4 Redis 故障；E5 单聊回归；E6 旧 250 拒绝。

### 开放问题
1. 通知合并 / 在线集合（P1/P2 性能优化）；
2. 撤回/删除：本期不支持；outbox 放弃已用 tombstone/deletedSeq 表达，删除复用同一语义；
3. 群管理员/禁言（本期 owner-only）；
4. 富媒体/搜索（本期 contentType=101）；
5. 200 人阈值/2000 上限按指标调整；
6. 多端（独立项目，本期单设备）。

---

## 15. 参考代码

| 主题 | 位置 |
|---|---|
| 单聊 outbox/发布 | `src/services/msg_service/outbox_worker.cpp`、`src/storage/mysqlhandler.cpp:1502-1805` |
| Mongo 消息读写 | `src/storage/mongohandler.cpp:39-330` |
| 单聊扇出/三态 | `src/services/push_service/server.cpp`、`src/services/gateway_router.cpp:259-443` |
| 单聊 ACK（仅对照） | `docs/design/messaging.md`、`src/gateway/ws_session.cpp:832-876` |
| 当前群实现（将替换） | `src/storage/mysqlhandler.cpp:1880-2319`、`src/services/group_service/server.cpp` |

---

## 附录 A：设计决策（ADR v3）

> 配套：[群聊设计](group-chat.md)、[协议与数据模型（附录 B）](#附录-b协议与数据模型v3)、[群聊测试](../testing/group-chat.md)
>
> v3 按已确认决策重写：Mongo 单副本、outbox→Kafka→PushService、≤200 内联、210 群同步、
> 直接切新协议、**无 ACK / 未读客户端本地**、单设备。本文记录**为什么**，以及被否决的方案。

---

### 0. 决策一览

| 编号 | 决策 | 理由 |
|---|---|---|
| ADR-01 | 群消息 **Mongo 单副本**，不迁旧 mock 数据 | 与单聊统一消息模型；单副本避免 O(N) 写放大 |
| ADR-02 | 每群 `im_group_stream` 热点控制行 | seq/epoch/解散准入一个串行点，避免锁 `im_groups` |
| ADR-03 | 发送事务性 outbox（op=4, stage 0/1）+ staging | 写库与写 Mongo/发 MQ 不能同事务；正文 staging，同事务可恢复 |
| ADR-04 | 扇出经 Kafka + PushService，尽力而为 | 发送与扇出解耦；Pull 才是正确性保证；避免阻塞分区 |
| ADR-05 | 不做每成员副本/每成员 outbox | 写放大 O(N)，2000 人群不可接受 |
| ADR-06 | **群没有 ACK** | 单副本 + Pull 已能恢复；“送达客户端”不是服务端可维护的事实 |
| ADR-07 | **未读客户端本地**，服务端零状态 | 单设备 MVP；不做 ACK 的自然结果 |
| ADR-08 | 小群 ≤200 内联 251，大群只 hint + Pull | 控制单帧体积；大群省带宽 |
| ADR-09 | 群同步用**新帧 210**，不扩展 209 | 群/单聊同步解耦；209 保持单聊语义 |
| ADR-10 | 群历史用 245 独立 RPC，无锁 | 读多写少，不走 250 命令/群行锁 |
| ADR-11 | 锁顺序：成员行 → stream 行 | 消除 Send/Leave/Join 环形等待 |
| ADR-12 | **直接切新协议**，不做旧 250 兼容 | 旧数据是 mock；降低双协议维护成本 |
| ADR-13 | 通知合并/在线集合/独立 fanout 延后 | 先保证正确与可理解，按指标演进 |
| ADR-14 | `published_seq` 可见水位 | `next_seq` 是分配水位，不能当可见/同步上界 |
| ADR-15 | 空洞写 tombstone + `deletedSeq` | outbox 放弃时永久洞必须可被客户端跳过 |
| ADR-16 | outbox payload 只存引用，正文放 MySQL staging | 避免 7 天正文重复；staging 同事务保证一致性 |
| ADR-17 | 单群 Kafka 串行；大群只 hint | 不牺牲 seq 顺序；大群不追求每条正文 |

---

### ADR-01 群消息 Mongo 单副本

**决策**

- Mongo `IM-System.msg`，一条群消息一份文档：`convID=groupID`、`isGroupMsg=true`、`recvID=0`、`sessionType=2`；
- 消息正文/内容类型/扩展统一 `sdkws.MsgData`；
- MySQL 只保留群元数据、seq、成员、outbox；
- 旧 mock 数据不迁移，直接清理重建。

**理由**

1. 与单聊统一消息模型：客户端只处理 `MsgData`，去重/排序/渲染逻辑复用；
2. 单副本：存储/写与成员数无关（O(1)/消息），避免写扩散；
3. Mongo 适合“只追加、按 `{convID,seq}` 范围读”的正文；
4. 不迁移：当前群数据是 mock，迁移成本无收益。

**代价**

- 发送后到 Mongo 落库有短暂窗口；Pull 可能暂时看不到该 seq（客户端 buffer）；
- 需要 group 专用 Mongo 查询（`isGroupMsg=true`）；
- 幂等窗口 = outbox 7 天（与单聊一致），过期后可能重复。

**被否决**

- 群消息继续存 MySQL：与单聊两套消息模型；正文与 seq 同事务虽强，但统一模型收益更大；
- 每成员 Mongo 副本：写放大 O(N)；
- 消息迁移：mock 数据，无价值。

---

### ADR-02 `im_group_stream` 热点控制行

**决策**：新增 `im_group_stream(group_id, next_seq, published_seq, last_msg_time, epoch, dismissed, updated_at)`；
发送只锁这一行 + 成员行；`im_groups` 回归读多写少。`published_seq` 由 ADR-14 增补。

**理由**

- 群内 seq 必须串行；epoch/解散准入与 seq 同一行，一个串行点足够；
- `UPDATE ... SET next_seq=LAST_INSERT_ID(next_seq+1) WHERE dismissed=0` 原子完成“准入+分配”；
- 避免 `Pull`/`List` 再拿 `im_groups` 行锁。

**代价**

- 单群吞吐受一行锁限制（配合群限流 50/s）；
- 需要新表/重置旧 `im_groups.last_seq`。

**被否决**：Redis INCR（持久化/回滚难）、seq 段预分配（空洞）、全局 seq（未读/分页/隔离复杂）。

---

### ADR-03 outbox op=4（两阶段）

**决策**

- 发送事务只做：成员锁 + stream seq + `im_group_send_staging(正文)` + `im_outbox(op=4, payload=引用, stage=0)`；
- worker：stage=0 从 staging 读正文写 Mongo(`committed=false`) → stage=1 → 删 staging；
  stage=1 `PublishMessage` + `published_seq` + Kafka → state=2；
- 不等待 ACK；Kafka 失败只重试发布；Mongo 放弃写 tombstone，不留不可见洞。

**理由**

- “MySQL 写”与“Mongo 写 + Kafka 发”不能同事务；outbox 是事务性发件箱；
- 发送延迟只含 MySQL 事务，不受 Mongo/Kafka 抖动影响；
- 状态可查（stage/state），可重试、可告警。

**代价**

- 发送响应早于 Mongo 可见，存在短暂 gap；
- outbox 多一列 stage 状态 + staging 表；
- stage=0 放弃必须写 tombstone（否则永久洞不可见）；stage=1 放弃消息已可见。

**被否决**：发送事务里同步写 Mongo（不能与 MySQL 原子）、直接发 Kafka（无重试）、
Redis 队列/staging（非权威，崩溃丢正文）。

---

### ADR-04 Kafka + PushService 扇出，尽力而为

**决策**：`group_topic`（key=groupID）+ PushService GroupFanout + `PushBatchBestEffort`；
无路由/实例否认 → NotOnline，不广播；实例不可达 → Unknown，不重试。

**理由**

- 发送与扇出解耦，扇出耗时不进发送延迟；
- Kafka 削峰/重试/顺序（同群同分区）；
- 一个群消息对每个网关最多一次 RPC，不是每成员一次；
- `PushBatch` 的广播兜底在 2000 人群 1900 离线时会产生 O(离线×实例) 的无效 RPC，群推送用 best-effort 避免；
- 群没有“每收件人必须送达”的语义，Pull 兜底。

**代价**

- 新增 topic/消费者/监控；
- 推送可能丢/重/乱序；
- PushService 需要读群消息/成员。

**被否决**：GroupService 同步扇出、网关本地扇出、每成员一个 Kafka 消息、Unknown 无限重试。

---

### ADR-05 不做每成员副本/每成员 outbox

**决策**：单副本 Mongo + 成员 joinSeq；不为每个成员写消息/outbox。

**理由**：写扩散 O(N)；群消息是共享一条流；单副本 + 范围读足够。

**代价**：成员可见性靠 joinSeq；没有逐成员送达状态（也不需要）。

---

### ADR-06 群没有 ACK

**决策**：群聊**不存在 ACK**。服务端不维护“消息是否送达某成员客户端”的任何状态；
不实现 `107`、`ack_seq`、逐群回执。

**理由**

1. 群消息是单副本，没有每成员收件箱；服务端没有“送达”事实可记；
2. 客户端收没收到只有客户端知道；上报也只是自称，不能证明送达；
3. 消息不丢靠 Mongo + Pull，不靠 ACK；
4. 逐群 ACK 只会引入额外写、重试、断连语义，收益为负；
5. “ACK 送达客户端”这个功能本身不应存在（你的判断，我同意）。

**代价/边界**

- 服务端无法回答“某成员是否收到/已处理”；这是有意的；
- 如果产品要“已读人数/已读回执”，那是独立的 read-receipt 功能，不是送达 ACK，另行设计；
- 客户端本地 cursor/已读位置只影响自己。

**被否决**：复用 106（绑定单聊 receipts/Mongo status）、继续 250 Ack（群行锁/幂等表）、107 只读水位（本期也不需要）。

---

### ADR-07 未读客户端本地

**决策**：服务端零未读状态；`Group` 不返回 unread/readSeq；
客户端本地保存每群 `lastReadSeq`，`unread = lastSeq - max(lastReadSeq, joinSeq)`。

**理由**

- 没有 ACK/已读上报，服务端没有权威 readSeq；
- 服务端算未读需要逐成员写或 COUNT，前者写放大、后者读放大；
- 本期单设备，本地未读足够。

**代价**

- 换设备/重装未读丢失；
- 无法在服务端做未读总数/红点聚合。

**后续**：多端/跨端未读是一个独立项目，届时再设计“已读同步”；
本期不预留 107、不做 read watermark。

**被否决**：每成员未读计数（O(N) 写）、服务端 COUNT（大群读放大）、把 ack 当 unread（语义不对）、本期只读水位（单设备不需要）。

---

### ADR-08 小群内联、大群 hint（200）

**决策**：`activeMembers<=200`（排除 sender 前）且消息≤256KiB → 251 带 MsgData；
否则只带 lastSeq/epoch，客户端 245。

**理由**：小群一次上屏；大群省带宽/单帧；阈值沿用现有 200 经验。

**代价**：大群多一次 Pull；阈值附近行为突变；需要客户端 hint 逻辑。

**被否决**：永远内联（大群不可控）、永远 hint（小群多一次 RTT）、按在线人数动态（需要在线集合）。

---

### ADR-09 群同步用新帧 210

**决策**：单聊 209 不变；群同步单独发 210 `GroupSyncSummary`（在 251 内联之后）。
不扩展 `SyncSummary.groupRanges`。

**理由**

- 群/单聊同步解耦：一个失败不影响另一个的“完成”信号；
- 209 保持单聊语义，不引入群字段；
- 客户端可以分别处理“单聊同步完成”和“群同步完成”；
- 比“把群塞进 209”更清晰，代价是多一个帧/等待点。

**代价**

- 客户端多一个 210 处理；
- 登录时可能多一帧；
- 若客户端只等 209，会漏掉群同步 → 需要客户端升级（直接切协议已接受）。

**被否决**：扩展 209（单聊/群耦合）、不做群同步（首屏差）、群并入 240（读模型改造大）。

---

### ADR-10 群历史用 245 独立 RPC

**决策**：`245 GetGroupChatHistory`，无锁读 Mongo；不走 250 命令。
支持 `afterSeq`（向后增量/补洞）、`beforeSeq`（向前翻旧；最新 N 用 `beforeSeq=lastSeq+1`）。
两者都为 0 表示从 `joinSeq` 向后拉，不是最新。

**理由**：Pull 读多写少；250 `Execute` 有群行锁/幂等表/事务语义，不适合高 QPS 读；
独立 RPC 可单独限流/超时/扩容；`snapshot`/`minSeq` 语义清晰；
只做 `afterSeq` 会导致大群首次加载从头拉，因此同时保留向前模式。

**代价**：新增帧/RPC；客户端需实现分页/快照。

**被否决**：继续 250 Pull（群锁）、复用 244（好友校验/convID 语义不兼容）。

---

### ADR-11 锁顺序：成员行 → stream 行

**决策**：Send/Leave/Kick/Review 统一先锁成员行（存在时），再锁 stream；
Dismiss 只锁 stream；Rename 只写 `im_groups`。

**理由**：发送与踢人/退群串行化在成员行；发送与入群串行化在 stream 行；无环。

**被否决**：先 stream 再成员（与发送相反，重入可能死锁）、不锁成员（踢人后仍可发送）、锁 `im_groups`（热点元数据行）。

---

### ADR-12 直接切新协议

**决策**：不兼容旧 `250 Send/Ack/Read/Pull`；250 只保留成员/元数据命令；
旧数据 mock 直接清理；不做双写/迁移/开关。

**理由**

- 旧群数据是 mock，迁移无收益；
- 双协议/双写会显著增加实现与测试成本；
- 新帧（210/245/246）与 MsgData 统一模型是 breaking change，长痛不如短痛；
- 旧 250 Send/Ack 的行为（群行锁、ack/read 字段）与新设计冲突。

**代价**

- 旧客户端必须同步升级；
- 不能灰度回滚到旧群链路（只能回滚代码版本 + 数据重建）；
- 需要一次性清理 mock 数据。

**被否决**：保留旧 250 映射（双语义/双写）、只保留 Send 兼容（ACK/Pull 仍冲突）、双协议并行一个版本（测试成本最高）。

---

### ADR-13 延后优化

| 优化 | 触发 | 代价 |
|---|---|---|
| 通知合并（coalescing） | 单群 >50 msg/s 或 egress 高 | per-group 定时器；客户端处理 hint |
| 在线成员集合 `grp:online:{gid}` | N 大、离线多 | 登录/登出 O(群数) 写 |
| `msg:{id}` 缓存 | 扇出读 Mongo QPS 高 | 缓存一致性 |
| 独立 group-push 服务 | 单进程 CPU 饱和 | 新角色/运维 |
| 群并入 240 会话列表 | 产品要求统一列表 | 读模型改造 |

---

### ADR-14 `published_seq` 可见水位

**决策**：`im_group_stream` 增加 `published_seq`（已发布最大 seq，含 tombstone）；
`Group.lastSeq`、210 的 `lastSeq`、245 上界都用 `published_seq`；`next_seq` 只表示已分配。

**理由**

- `next_seq` 可能领先 Mongo committed；用它做同步上界会“说已最新、实际还差一条”；
- `published_seq` 由 worker 在 `PublishMessage`/tombstone 后 `GREATEST` 推进；
- `needsPull = (toSeq < lastSeq)`，内联没覆盖到 published 就必须 Pull。

**代价**

- worker 多一次 stream 更新；
- `published_seq` 是“最大已发布”，不是连续可见水位（可能有中间 gap）；
- 客户端仍然只能用实际拉到的连续消息推进 cursor。

**被否决**：用 `next_seq` 当 lastSeq（当前缺陷）；维护严格连续 `visible_seq`（需要额外扫描/修复，先不做）。

---

### ADR-15 空洞 tombstone + `deletedSeq`

**决策**：outbox stage=0 超过最大尝试时，写一条 `isDeleted=true`、`committed=true` 的 MsgData 文档；
更新 `published_seq`；`245` 返回 `isDeleted` 消息和 `deletedSeq`（本页最大空洞）；客户端跳过正文但推进 cursor。

**理由**

- 否则 seq 已分配、Mongo 无文档，客户端会看到永久洞且无法表达；
- tombstone 让“永久缺失”成为可拉取、可跳过的数据；
- `deletedSeq` 给客户端/监控一个明确信号；未来撤回/删除复用同一语义。

**代价**

- stage=0 超限先查 Mongo：已存在未发布则 Publish，不存在才写 tombstone；
  超限后不再尝试写正文、只重试 tombstone；tombstone 写失败继续重试，不能 state=3 了事；
- tombstone 没有正文，占用一个文档；
- 本期不支持撤回/删除，只有 outbox 放弃会产生 tombstone。

**被否决**：只告警/人工修（客户端无法跳过）；把 seq 回退（会破坏已分配顺序）；让客户端无限重试（永久洞永远补不上）。

---

### ADR-16 outbox payload 只存引用，正文放 MySQL staging

**决策**：发送事务同时写 `im_group_send_staging`（正文 MsgData JSON）和 `im_outbox`（payload 只存 `serverMsgID` 引用）；
worker 从 staging 读正文写 Mongo，stage=1 后删除 staging；outbox 不保留正文 7 天。

**理由**

- outbox 保留 7 天，若 payload 存完整正文会造成长期双份存储；
- staging 与 seq/outbox 在**同一 MySQL 事务**，不存在跨存储一致性；
- worker 失败可重试；staging 删除后仍可从 Mongo 恢复。

**代价**

- 多一张表/清理任务；
- worker 多一次 staging 读/删；
- outbox 行本身不能独立恢复正文（需 staging/Mongo）。

**被否决**：outbox 存完整 payload（7 天重复存储）；Redis staging（非权威，崩溃丢正文）；正文只放 Kafka（非存储）。

---

### ADR-17 单群 Kafka 串行，大群只 hint

**决策**：`group_topic` 仍以 `groupID` 为 key，单群固定一个分区、串行消费；
**不**按 seq bucket 拆分区；大群只推 hint，客户端 Pull；在线集合/成员分片留到下一阶段。

**理由**

- 不牺牲同群顺序；seq 是客户端排序/缺口检测的基础；
- 大群每条都推正文会放大 egress，且单 lane 也扛不住；
- 先保证正确与可理解，热群/大群按指标再优化。

**代价**

- 单群 fanout 无法组内并行，单 lane 是上限；
- 2000 人群 50 msg/s 时依赖 hint + Pull；
- 在线集合/coalescing 延后，离线成员仍做路由查询。

**被否决**：groupID+seq bucket 多分区（破坏顺序）；现在就在大群推正文；现在做在线集合/独立 fanout 服务。

---

### 14. 与旧群方案对照

| 议题 | 旧方案（未落地） | v3 | 原因 |
|---|---|---|---|
| 消息存储 | MySQL `im_group_messages` | Mongo 单副本 MsgData | 统一消息模型、单副本 |
| 发送 | 事务内写消息+last_seq；best-effort produce | stream seq + outbox(op=4) | 可重试、可观测 |
| 投递 | Kafka group-events + im_group_delivery 租约 + 2s 扫描 | group_topic 每消息事件 + PushService | 无租约/扫描，链路更短 |
| ACK | 250 Ack，6 条规则 | 不存在 | 单副本+Pull 已够；“送达客户端”不应由服务端维护 |
| 未读 | last_seq - ack_seq | 客户端本地 | 无 ACK 的自然结果 |
| 拉取 | 250 Pull（群行锁） | 245 无锁 Mongo | 读路径不加锁 |
| 大群 | ≤500 内联 | ≤200 内联 | 沿用现有阈值 |
| 兼容 | 渐进 | 直接切 | mock 数据、降低双协议成本 |

---

### 15. 实现与理解成本

**新增**：`im_group_stream`（含 `published_seq`）；`im_group_send_staging` 1 表；
Mongo 群查询 2 方法 + tombstone；210/245/246 3 帧；Send/History 2 RPC；
group_topic + fanout consumer；op=4 worker；成员缓存；配置约 22 项。

**删除**：`im_group_messages`；`im_groups.last_seq`；成员 `ack_seq/read_seq`；
2s Deliver 轮询、MIN(ack_seq) 判待投、per-group Notice 拼装、30s 离线缓存；
旧 250 Send/Ack/Read/Pull；双写/兼容开关。

**净成本**：约 +700/-500 行；核心概念 4 个（stream、outbox stage、Kafka fanout、Mongo 单副本），
删掉 6 个旧概念（轮询、min-ack、notice 拼装、offline cache、群水位/租约、ACK 清理）。

---

### 16. 风险

| 风险 | 缓解 |
|---|---|
| 发送后 Mongo 未落库的短暂 gap | 客户端本地回显 + buffer/245 重试；worker 快速重试 |
| Mongo 长期不可用导致消息缺失 | stage=0 超过最大尝试必须写 tombstone；tombstone 失败继续重试；告警 |
| `published_seq` 落后/不连续 | worker 成功后 GREATEST 推进；同步 `needsPull=(toSeq<lastSeq)`；客户端只按实际消息推进 cursor |
| staging 孤儿/丢失 | staging 与 outbox 同事务；stage=1 删除；孤儿按 24h 清理；丢失时回读 Mongo |
| Kafka/Push 故障漏推 | Pull 兜底；210/245 补齐 |
| 大小群阈值体验 | 可配置；测试 200/201 |
| 无 ACK/未读的跨端问题 | 本期单设备，可接受；多端/跨端未读独立立项 |
| 直接切的兼容风险 | 明确 breaking release；客户端同步升级 |

---

## 附录 B：协议与数据模型（v3）

> 配套：[群聊设计](group-chat.md)、[群聊测试](../testing/group-chat.md)、[设计决策（附录 A）](#附录-a设计决策adr-v3)
>
> 本文给出 v3 可直接落地的 proto、Mongo/MySQL 结构、关键 SQL、Redis/Kafka 约定、配置与重置步骤。
> v3 是**直接切新协议**：不兼容旧 `250 Send/Ack/Read/Pull`；旧 mock 数据不迁移。

---

### 1. WebSocket 帧

| type | 方向 | 载体 | 说明 |
|---|---|---|---|
| 90/91 | C→S | 注册/登录 | 不变；91 的 `cursors` 带每群 cursor |
| 101/104/106 | — | 单聊 | 不变；群不复用；群没有 ACK 帧 |
| 200/209 | S→C | 单聊离线/同步结束 | 不变 |
| **210** | S→C | `groupchat.GroupSyncSummary` | 群同步结束 |
| **245** | C→S | `groupchat.GetGroupChatHistoryReq` | 群历史请求 |
| **245** | S→C | `groupchat.GetGroupChatHistoryResp` | 群历史响应 |
| **246** | C→S | `groupchat.SendGroupMessageReq` | 群发送请求 |
| **246** | S→C | `groupchat.SendGroupMessageResp` | 群发送响应 |
| 250 | C→S | `groupchat.Command` | 只保留成员/元数据/申请命令 |
| 250 | S→C | `groupchat.Reply` | 命令响应 |
| 251 | S→C | `groupchat.Notice` | 新消息/同步内联/刷新/解散 |

- 群没有 ACK/107；未读由客户端本地维护；
- 旧 `250 Send/Ack/Read/Pull` 不再支持，网关返回 `UNSUPPORTED_OPERATION`；
- 群消息统一用 `sdkws.MsgData`，`groupchat.Message` 删除。

---

### 2. Proto

#### 2.1 `proto/groupchat.proto`

```proto
syntax = "proto3";
package groupchat;
import "sdkws.proto";

enum Operation {
  List = 0; Create = 1; Search = 2; Apply = 3; Applications = 4;
  Review = 5; Members = 6; Rename = 7; Leave = 8; Kick = 9;
  Dismiss = 10; MyApplications = 15;
  // 11 Send / 12 Pull / 13 Ack / 14 Read 已删除；旧值返回 UNSUPPORTED_OPERATION
}

message Group {
  string id = 1;
  string name = 2;
  uint64 owner = 3;
  uint64 memberCount = 4;
  bool dismissed = 5;
  bool joined = 6;
  uint64 joinSeq = 7;      // 成员可见下界
  uint64 lastSeq = 8;      // = published_seq（已发布最大，不是 next_seq）
  bool large = 11;         // memberCount > GROUP_INLINE_MAX_MEMBERS
  uint64 epoch = 12;
  string number = 13;      // 10 位群号
  uint64 lastMsgTime = 14;
  // 9 readSeq / 10 unread 已删除（未读客户端本地维护）
}

message Member { uint64 account = 1; string nickname = 2; bool owner = 3; }

message Application {
  uint64 id = 1; string groupID = 2; uint64 account = 3; string nickname = 4;
  string reason = 5; int32 status = 6; int64 time = 7;
  string groupName = 8; string groupNumber = 9;
}

message Command {
  Operation operation = 1;
  uint64 actor = 2;        // 网关覆盖
  string groupID = 3;
  string text = 4;
  uint64 target = 5;
  uint64 applicationID = 6;
  bool accept = 7;
  uint64 after = 8;
  uint32 limit = 9;
  string clientID = 11;
  uint64 epoch = 12;
}

message Reply {
  string errorCode = 1;
  string errorMessage = 2;
  repeated Group groups = 3;
  repeated Member members = 4;
  repeated Application applications = 5;
  uint64 cursor = 7;
  bool hasMore = 8;
  string groupID = 10;
  uint64 epoch = 11;
  // 6 messages / 9 snapshot 已删除（群消息走 245）
}

// type 251
message Notice {
  string groupID = 1;
  uint64 lastSeq = 2;
  repeated sdkws.MsgData messages = 3; // 小群内联；大群为空
  bool refresh = 4;
  uint64 epoch = 5;
  int32 reason = 6;        // 0=新消息 1=刷新 2=解散 3=被踢
  uint64 lastMsgTime = 7;
}

// type 246
message SendGroupMessageReq {
  string groupID = 1;
  string clientID = 2;
  bytes content = 3;
  int32 contentType = 4;   // 0 视为 101
  string ext = 5;
  uint64 actor = 6;        // 网关覆盖
}
message SendGroupMessageResp {
  string errorCode = 1;
  string errorMsg = 2;
  sdkws.MsgData msg = 3;
}

// type 245
message GetGroupChatHistoryReq {
  uint64 ownerUserID = 1;  // 网关覆盖
  string groupID = 2;
  int64 afterSeq = 3;      // 向后：seq > afterSeq；0=joinSeq
  int32 limit = 4;         // 1..100，默认 20
  int64 snapshot = 5;      // 0=请求时 published_seq
  int64 beforeSeq = 6;     // 向前：seq < beforeSeq；与 afterSeq 互斥
}
message GetGroupChatHistoryResp {
  repeated sdkws.MsgData messages = 1;
  bool hasMore = 2;
  int64 nextSeq = 3;
  int64 minSeq = 4;        // = joinSeq
  int64 lastSeq = 5;       // = published_seq（或 snapshot 截断）
  uint64 epoch = 6;
  int64 deletedSeq = 7;    // 本页最大 tombstone seq；0=无；客户端以 isDeleted 为准
}

// type 210
message GroupSyncRange {
  string groupID = 1;
  int64 fromSeq = 2;       // floor+1
  int64 toSeq = 3;         // 实际内联末条；无内联=fromSeq-1
  int64 lastSeq = 4;       // = published_seq
  int64 joinSeq = 5;
  uint64 epoch = 6;
  bool hasGap = 7;
  bool needsPull = 8;
}
message GroupSyncSummary {
  repeated GroupSyncRange groups = 1;
  uint32 messagesPushed = 2;
  bool hasMore = 3;
}

service GroupService {
  rpc Execute(Command) returns (Reply);
  rpc SendGroupMessages(SendGroupMessageReq) returns (SendGroupMessageResp);
  rpc GetGroupChatHistory(GetGroupChatHistoryReq) returns (GetGroupChatHistoryResp);
}
```

`GroupSyncRange.fromSeq/toSeq`：`floor=max(cursor,joinSeq)`，`fromSeq=floor+1`；
`toSeq` = 实际内联到的最后一条 seq；没有内联时为 `fromSeq-1`（空覆盖）。
`needsPull = (toSeq < lastSeq)`；`hasGap = fromSeq > cursor+1`（即 `cursor<joinSeq`）。
同步内联受 `GROUP_INLINE_MAX_BYTES` 限制：按字节截断，0 条能放下则 `needsPull`。

**gRPC 截止**：Send 3s、History 5s、Execute 10s。
**失败**：Send 回 246 错误；History 回 245 错误；均不改变连接。

#### 2.2 `proto/sdkws.proto`

**不新增** `SyncSummary.groupRanges`；群同步由 210 独立承载。`MsgData` 不变：
`sendID/recvID/convID/clientMsgID/serverMsgID/sessionType/contentType/content/seq/sendTime/isGroupMsg/ext`。

---

### 3. Mongo（消息正文）

#### 3.1 集合与文档

```js
// IM-System.msg
{
  serverMsgID: "<uuid>",
  sendID: 1001,
  recvID: 0,                 // 群消息固定 0
  convID: "<groupID>",
  clientMsgID: "<clientID>",
  senderPlatformID: 0,
  senderNickname: "A",
  senderFaceURL: "",
  sessionType: 2,            // 群聊
  msgFrom: 100,
  contentType: 101,
  content: BinData(...),     // MsgData.content
  seq: 42,
  sendTime: 1760000000000.0,
  status: 1,
  isGroupMsg: true,
  isDeleted: false,           // tombstone 置 true
  ext: "",
  committed: false            // worker 写时 false；PublishMessage 后 true
}
```

#### 3.2 索引

| 索引 | 用途 |
|---|---|
| `{convID:1, seq:1}`（已有） | 群/单聊按会话范围拉取 |
| `{serverMsgID:1}` unique（已有） | 按 ID 查消息、幂等 upsert |
| `{convID:1, isGroupMsg:1, seq:1}`（建议新增） | 群查询过滤更稳 |

单聊文档 `recvID!=0`、`isGroupMsg=false`；群文档 `recvID=0`、`isGroupMsg=true`，同集合共存。

**tombstone 文档**：outbox 放弃 Mongo 写时写一条 `isDeleted=true`、`committed=true`、`content` 为空、
`serverMsgID="hole-{gid}-{seq}"` 的文档；客户端跳过正文但推进 cursor。

#### 3.3 可见性

- 发送后 worker 先 `SaveMsgToMongoAsync(msg, false)`（`committed=false`）；
- `PublishMessage(serverMsgID)` 置 `committed=true`；成功（含 tombstone）后 `published_seq=GREATEST(published_seq,seq)`；
- `245`/同步只查 `committed != false`，即发布后可见；上界用 `published_seq`，不用 `next_seq`；
- 发送响应早于可见，客户端本地回显/缓存，Pull 短暂 gap 用退避重试；
- 永久空洞（outbox 放弃）写 tombstone，`245` 返回 `isDeleted` + `deletedSeq`。

#### 3.4 需要新增的 Mongo 方法

```text
GetGroupChatHistoryAsync(groupID, afterSeq, limit)      // isGroupMsg=true, committed!=false, seq>after, asc
GetGroupLatestMessagesAsync(groupID, afterSeq, limit)   // 同上，desc 取最近 N 再反转（同步用）
```

`SaveMsgToMongoAsync`、`PublishMessage`、`GetMsgByServerMsgIDAsync` 复用现有实现。

---

### 4. MySQL

#### 4.1 新 schema（无 `im_group_messages`）

```sql
CREATE TABLE im_groups (
  id BIGINT UNSIGNED PRIMARY KEY,
  name VARCHAR(256) NOT NULL,
  owner BIGINT UNSIGNED NOT NULL,
  member_count BIGINT UNSIGNED NOT NULL DEFAULT 1,
  dismissed TINYINT NOT NULL DEFAULT 0,
  created_time BIGINT NOT NULL DEFAULT 0,
  updated_at BIGINT NOT NULL DEFAULT 0,
  KEY group_owner(owner)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE im_group_stream (
  group_id BIGINT UNSIGNED PRIMARY KEY,
  next_seq BIGINT UNSIGNED NOT NULL DEFAULT 0,      -- 已分配最大 seq
  published_seq BIGINT UNSIGNED NOT NULL DEFAULT 0, -- 已发布最大 seq（含 tombstone）
  last_msg_time BIGINT UNSIGNED NOT NULL DEFAULT 0,
  epoch BIGINT UNSIGNED NOT NULL DEFAULT 1,
  dismissed TINYINT NOT NULL DEFAULT 0,
  updated_at BIGINT NOT NULL,
  CONSTRAINT fk_stream_group FOREIGN KEY(group_id) REFERENCES im_groups(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE im_group_members (
  group_id BIGINT UNSIGNED NOT NULL,
  account BIGINT UNSIGNED NOT NULL,
  active TINYINT NOT NULL DEFAULT 1,
  epoch BIGINT UNSIGNED NOT NULL DEFAULT 1,
  join_seq BIGINT UNSIGNED NOT NULL DEFAULT 0,
  joined_time BIGINT NOT NULL DEFAULT 0,
  left_time BIGINT NOT NULL DEFAULT 0,
  PRIMARY KEY(group_id, account),
  KEY account_active(account, active, group_id),
  CONSTRAINT fk_member_group FOREIGN KEY(group_id) REFERENCES im_groups(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 沿用：im_group_numbers、im_group_number_cursor、im_group_applications

CREATE TABLE im_group_send_staging (
  server_msg_id VARCHAR(64) PRIMARY KEY,
  group_id BIGINT UNSIGNED NOT NULL,
  seq BIGINT UNSIGNED NOT NULL,
  payload MEDIUMTEXT NOT NULL,        -- MsgData JSON；与 outbox 同事务写入
  created_at BIGINT NOT NULL,
  updated_at BIGINT NOT NULL,
  KEY staging_retention(updated_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE im_group_requests (
  actor BIGINT UNSIGNED NOT NULL,
  client_id VARCHAR(64) NOT NULL,
  payload MEDIUMTEXT NOT NULL,
  reply MEDIUMTEXT NOT NULL,
  created_time BIGINT NOT NULL DEFAULT 0,
  updated_at BIGINT NOT NULL DEFAULT 0,
  PRIMARY KEY(actor, client_id),
  KEY request_retention(updated_at)
) ENGINE=InnoDB DEFAULT CHARSET=ascii;
```

> 不保留 `im_groups.last_seq`（权威在 stream）；不保留成员 `ack_seq/read_seq`（无 ACK）。
> 老库直接 DROP 相关表后重建（mock 数据不迁移）。

#### 4.2 `im_outbox`（op=4）

| 字段 | op=4 |
|---|---|
| operation | 4 |
| dedupe_key | `g:{groupID}:{sender}:{clientID}` |
| actor_account / peer_account | sender / 0 |
| conversation_id | groupID |
| server_msg_id / seq | 消息 ID / seq |
| payload | 只存引用：`{"serverMsgID":"..."}` 或空；正文在 `im_group_send_staging` |
| stage | 0=未写 Mongo；1=Mongo 已写（committed=false） |
| state | 0 待处理 / 1 已领取 / 2 完成 / 3 放弃 |
| dispatch_done | 不使用（保持 0） |

建议新增索引：`KEY outbox_op_ready(operation,state,available_at,id)`。

`ClaimOutboxTask` 按 operation 过滤：MsgService `{1,2,3}`，GroupService `{4}`。

> staging 与 outbox 在**同一 MySQL 事务**写入：不会出现“有 outbox 无正文/有正文无 outbox”。
> worker 写 Mongo 成功后删除 staging；staging 不保留 7 天，避免正文重复存储。

---

### 5. 关键 SQL

#### 5.1 发送事务

```sql
START TRANSACTION; -- READ COMMITTED

SELECT id,server_msg_id,seq FROM im_outbox WHERE operation=4 AND dedupe_key=?;
-- 命中：从 im_group_send_staging / Mongo 取 MsgData 返回

SELECT active FROM im_group_members
 WHERE group_id=? AND account=? FOR UPDATE;

UPDATE im_group_stream
   SET next_seq=LAST_INSERT_ID(next_seq+1), last_msg_time=?, updated_at=?
 WHERE group_id=? AND dismissed=0;
SELECT LAST_INSERT_ID();

INSERT INTO im_group_send_staging(server_msg_id,group_id,seq,payload,created_at,updated_at)
VALUES (?,?,?,?,?,?);

INSERT INTO im_outbox
 (operation,dedupe_key,actor_account,peer_account,conversation_id,
  server_msg_id,seq,payload,stage,created_at,updated_at)
VALUES (4,?,?,0,?,?,?,'{"serverMsgID":"..."}',0,?,?);

COMMIT;
```

失败：
- 成员无/`active=0` → `NOT_MEMBER`；
- stream affected=0 → `GROUP_NOT_FOUND`/`GROUP_DISMISSED`；
- outbox 唯一键冲突 → ROLLBACK（seq 回滚）→ 从 staging/Mongo 读获胜者 → 返回同一消息；
- 其他 → `DB_ERROR`，无半写；staging 与 outbox 同事务，不会半写。

#### 5.2 outbox worker

```text
claim op=4 (state=0/lease 过期, FOR UPDATE SKIP LOCKED)
if stage=0:
  msg = read staging by server_msg_id (缺失则读 Mongo)  // 失败 -> retry
  SaveMsgToMongoAsync(msg, committed=false)             // 失败 -> retry
  UPDATE im_outbox SET stage=1
  DELETE FROM im_group_send_staging WHERE server_msg_id=?
if stage=1:
  PublishMessage(server_msg_id)                         // committed=true
  UPDATE im_group_stream SET published_seq=GREATEST(published_seq, seq)
  producer.deliverConfirmed(groupID, traceparent|serverMsgID)
  UPDATE im_outbox SET state=2
if stage=0 and attempts > GROUP_PUBLISH_MAX_ATTEMPTS:
  write tombstone(committed=true,isDeleted=true)
  UPDATE im_group_stream SET published_seq=GREATEST(published_seq, seq)
  DELETE staging; UPDATE im_outbox SET state=3  // tombstone 失败则继续重试
```

- Kafka 失败：`state=0, available_at=now+5s`，stage 仍 1；
- Mongo 失败：stage 0，重试；
- stage=0 超过最大尝试：先查 Mongo——已存在 `committed=false` → `PublishMessage` + `published_seq` + state=2；
  不存在 → **写 tombstone**（`$set isDeleted=true, committed=true`，upsert）+ `published_seq` + state=3；
  已存在 `committed=true` → `published_seq` + state=3；tombstone 写失败继续重试，绝不留下不可见空洞；超过最大尝试后不再尝试写正文，只重试 tombstone；
- stage=1 超过最大尝试：消息已 `committed`，可 Pull，`state=3` 告警即可，不写 tombstone；
- `published_seq` 在 Publish/tombstone 成功后推进；staging 成功后删除，孤儿按保留期清理。

#### 5.3 群列表/同步查询

```sql
SELECT g.id,g.name,g.owner,g.member_count,g.dismissed,
       s.published_seq AS last_seq,s.last_msg_time,s.epoch,
       m.join_seq
  FROM im_group_members m
  JOIN im_group_stream s ON s.group_id=m.group_id
  JOIN im_groups g ON g.id=m.group_id
 WHERE m.account=? AND m.active=1 AND g.dismissed=0
 ORDER BY s.last_msg_time DESC
 LIMIT ?;   -- GROUP_SYNC_MAX_GROUPS+1
```

#### 5.4 成员数维护

```sql
UPDATE im_groups
   SET member_count=(SELECT COUNT(*) FROM im_group_members WHERE group_id=? AND active=1),
       updated_at=?
 WHERE id=?;
```

成员变更事务内执行；不在发送路径。

#### 5.5 群历史（Mongo 伪代码）

```text
lastSeq = published_seq
 to = min(snapshot>0?snapshot:lastSeq, lastSeq)

// 向后（补洞/增量）
from = max(afterSeq, joinSeq)
find({convID, isGroupMsg:true, committed:{$ne:false}, seq:{$gt:from,$lte:to}})
  .sort({seq:1}).limit(limit+1)

// 向前（最新/翻旧）：仅 beforeSeq>0；最新 N 用 beforeSeq=to+1
upper = min(beforeSeq, to+1)
find({convID, isGroupMsg:true, committed:{$ne:false}, seq:{$lt:upper,$gt:joinSeq,$lte:to}})
  .sort({seq:-1}).limit(limit+1)   // 取到后反转成升序
```

- `afterSeq` 与 `beforeSeq` 同时 >0 → `INVALID_PARAM`；
- 向后：`nextSeq=最后一条 seq`（空页 `from`）；向前：`nextSeq=最早一条 seq`（空页 `joinSeq`）；
- `lastSeq=to`、`minSeq=joinSeq`、`deletedSeq=本页最大 isDeleted seq`（无则 0）；
- tombstone（`isDeleted=true`）也返回，客户端跳过正文但推进 cursor；
- 本期不支持撤回/删除；永久空洞只来自 outbox 放弃 → tombstone；其余同 README §8。

---

### 6. Redis / Kafka

#### 6.1 Redis

| key | 类型 | 值 | TTL |
|---|---|---|---|
| `grp:members:{gid}` | HASH | account→joinSeq | 600s，成员变更 DEL |
| `grp:rate:m:{gid}:{uid}` | STRING | 1s 计数 | 1s |
| `grp:rate:g:{gid}` | STRING | 1s 计数 | 1s |
| `gw:user:{account}` | STRING | instanceId:sessionId | 300s |
| `msg:{serverMsgID}` | STRING | MsgData（缓存，可选） | 1h~7d |

#### 6.2 Kafka

| topic | key | payload | 消费组 |
|---|---|---|---|
| `msg_topic`（已有） | convID | `traceparent` + `serverMsgID`（竖线分隔） | `push-service-group` |
| `group_topic`（新增） | groupID | `traceparent` + `serverMsgID`（竖线分隔） | `push-group-fanout` |

`group_topic` 建议 32 分区（≥PushService 实例×2），首次部署为空；重置消费组会从 earliest 重放，
客户端按 serverMsgID 去重。

---

### 7. 配置

| 变量 | 默认 | 说明 |
|---|---|---|
| `GROUP_MAX_MEMBERS` | 2000 | 单群上限 |
| `GROUP_MAX_MSG_BYTES` | 65536 | 内容上限 |
| `GROUP_INLINE_MAX_MEMBERS` | 200 | active 成员数（排除 sender 前）超过则 hint |
| `GROUP_INLINE_MAX_BYTES` | 262144 | 内联 251 上限 |
| `GROUP_SEND_RATE_MEMBER` | 5 | 成员/群/秒 |
| `GROUP_SEND_RATE_GROUP` | 50 | 群/秒 |
| `GROUP_HISTORY_DEFAULT_LIMIT` | 20 | 245 默认 |
| `GROUP_HISTORY_MAX_LIMIT` | 100 | 245 上限 |
| `GROUP_SYNC_MAX_GROUPS` | 50 | 同步群数 |
| `GROUP_SYNC_MESSAGES_PER_GROUP` | 20 | 每群内联 |
| `GROUP_SYNC_MAX_MESSAGES` | 200 | 同步总消息 |
| `GROUP_MEMBERS_CACHE_TTL` | 600 | 成员缓存 |
| `GROUP_FANOUT_CHUNK` | 500 | 每批账号 |
| `GROUP_FANOUT_RETRY` | 3 | 扇出基础设施重试 |
| `GROUP_OUTBOX_RETENTION_DAYS` | 7 | op=4 完成行保留 |
| `GROUP_REQUEST_RETENTION_DAYS` | 7 | requests 保留 |
| `GROUP_PUBLISH_RETRY_MS` | 5000 | 发布重试基线 |
| `GROUP_PUBLISH_MAX_ATTEMPTS` | 30 | 超过 state=3 |
| `GROUP_OUTBOX_WORKERS` | MySQL pool 数 | worker 数 |
| `GROUP_STAGING_RETENTION_HOURS` | 24 | 孤儿 staging 清理 |
| `GROUP_TOPIC` | `group_topic` | topic |
| `GROUP_TOPIC_PARTITIONS` | 32 | 分区 |
| `GATEWAY_PUSH_ADDR` / `GATEWAY_ROUTING` | 已有 | 路由 |

---

### 8. 错误码

| 错误码 | 触发 |
|---|---|
| `0` | 成功 |
| `INVALID_PARAM` / `INVALID_GROUP` / `INVALID_REQUEST_ID` | 参数 |
| `GROUP_NOT_FOUND` / `GROUP_DISMISSED` / `NOT_MEMBER` | 群/成员 |
| `OWNER_REQUIRED` / `OWNER_CANNOT_LEAVE` / `ALREADY_JOINED` | 权限 |
| `APPLICATION_NOT_FOUND` / `APPLICATION_HANDLED` | 申请 |
| `EMPTY_MESSAGE` / `MESSAGE_TOO_LARGE` / `IDEMPOTENCY_CONFLICT` | 消息 |
| `RATE_LIMITED` / `GROUP_FULL` | 限流/上限 |
| `MEMBERSHIP_CHANGED` | epoch 不匹配 |
| `UNSUPPORTED_OPERATION` | 旧 250 Send/Ack/Read/Pull |
| `DB_ERROR` / `CACHE_ERROR` / `SERVICE_UNAVAILABLE` | 依赖 |

---

### 9. 直接切换/重置

1. 停服务；
2. DROP `im_groups/im_group_stream/im_group_members/im_group_messages/im_group_numbers/
   im_group_applications/im_group_requests/im_group_send_staging`（mock 数据不迁移）；
3. 删除 Mongo `msg` 中 `isGroupMsg=true` 的 mock 文档；
4. 按新 proto 重新生成代码；
5. 启动时 `InitializeGroups()` 重建 schema；
6. 客户端切换到 246/245/210；旧 250 操作返回 `UNSUPPORTED_OPERATION`。

**兼容**：无旧协议兼容、无双写、无回滚开关；这是 breaking release。

---

### 10. 运维检查

- [ ] `group_topic` 已创建（32 分区），`push-group-fanout` 可订阅；
- [ ] MySQL 新 schema 已重建，无 `im_group_messages`；`published_seq`/staging 存在；
- [ ] Mongo `{convID,seq}` / unique `{serverMsgID}` 索引存在；
- [ ] `group` 角色初始化 Mongo（`RoleNeedsMongo`/`InitStorage`）；否则历史/outbox worker 无 Mongo；
- [ ] GroupService 注入 `KafkaProducer(group_topic)` 并启动 op=4 worker；
- [ ] `kGroupMethods` 加 `SendGroupMessages/GetGroupChatHistory`，`GroupClient` 实现；
- [ ] PushService 启动第二个 consumer；
- [ ] 网关支持 210/245/246，旧 250 op 拒绝；
- [ ] 指标：outbox 积压、publish 失败、fanout latency、unknown 比例、seq 等待；
- [ ] 旧 mock 数据已清理（MySQL group 表 + Mongo `isGroupMsg=true`）。