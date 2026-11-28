# 群聊测试用例

> 状态：现状  
> 权威性：本文是「群聊 v3 测试用例与边界」的唯一来源  
> 最后核对：2026-11-26 @ `03ed7c2` + 工作区改造  
> 适用代码：scripts/smoke/group_test.py、scripts/run_group_tests.sh、test/

> 配套：[群聊设计](../design/group-chat.md)（v3 架构）、[群聊设计（附录 B）](../design/group-chat.md#附录-b协议与数据模型v3)（proto/DDL/帧）、[群聊设计（附录 A）](../design/group-chat.md#附录-a设计决策adr-v3)（ADR）
>
> 状态：**v3 测试设计**，对应已实现的 v3 链路。除 `[现状]`（单聊回归）外，
> 用例均为 `[设计]`；当前由 `scripts/smoke/group_test.py`（E2E）与
> `test/group_integration.cpp`（MySQL schema/发送事务/成员/seq）覆盖核心路径。
>
> v3 口径：消息在 Mongo `IM-System.msg` 单副本；发送 `246`；拉取 `245`；群同步结束 `210`；
> `209` 只表示**单聊**同步结束；旧 `250 Send/Ack/Read/Pull` 返回 `UNSUPPORTED_OPERATION`；
> **群没有 ACK/107；未读客户端本地**；不保留 ACK 附录，不把“送达客户端”当服务端状态。
>
> v3 修订（published_seq / tombstone / staging）：`im_group_stream.published_seq` 是可见水位
> （已发布最大 seq，含 tombstone），`next_seq` 只是分配水位；`Group.lastSeq`、210 的 `lastSeq`、
> 245 的上界一律用 `published_seq`，且 `needsPull = (toSeq < lastSeq)`。发送事务把正文写
> `im_group_send_staging`，outbox `payload` 只存 `serverMsgID` 引用；worker 写 Mongo(`stage=1`)
> 后删除 staging。outbox 放弃 Mongo 写时写 `isDeleted=true, committed=true` 的 tombstone 并推进
> `published_seq`，245 用 `deletedSeq` 提示本页最大空洞；客户端跳过正文但推进 cursor。
> 客户端前台每 30s 用 `245{afterSeq=cursor,limit=20}` 对账，且只能用实际拉到的连续消息推进 cursor。
>
> 标记：`[设计]` 待实现后自动化；`[现状]` 当前代码应通过；`[待定]` 方案 B，不纳入本期。
> 优先级：`P0` 阻断发布；`P1` 重要边界/故障；`P2` 性能/规模/体验。

---

## 1. 测试范围

| 范围 | 覆盖 | 核心编号 |
|---|---|---|
| 发送 | 246 校验、stream seq、事务原子性（staging+outbox）、幂等、并发、竞态、限流 | SND、IDM |
| 投递 | outbox stage 0/1/state 2/3、staging、Mongo 可见性、`published_seq`、Kafka、PushService、大小群、三态 | FAN、FLT |
| 拉取 | 245 分页、snapshot、joinSeq/minSeq、committed 过滤、tombstone/`deletedSeq`、跨会话隔离 | PULL |
| 登录同步 | 209 后 251 内联 + 210、`published_seq`/needsPull、上限、PARTIAL | SYNC |
| 成员 | 创建/申请/审批/退群/踢人/解散/改名/重入/epoch/并发 | MEM |
| 故障 | MySQL/Mongo/Kafka/Redis/Push/Group/Gateway/扇出崩溃、outbox 放弃 | FLT |
| 性能 | 发送吞吐、大群扇出、同步、分页、长稳 | PERF |
| 回归/安全 | 单聊、250 成员命令、旧 250 op 拒绝、越权/注入/刷屏 | REG、SEC |

**本期不做**：群 ACK、服务端未读、多端未读、**用户侧撤回/删除**、搜索、管理员/禁言、富媒体、240 统一会话列表。
**明确不做**：不写 `im_group_messages`（已删除）；不以 `209` 承载群同步；不做 ACK/107；
永久空洞只由 outbox 放弃 → tombstone 表达（不是用户删除）。

---

## 2. 测试环境

### 2.1 服务与依赖

| 组件 | 要求 |
|---|---|
| backbon / auth / group / msg / push | 各 1 实例；group 注入 Kafka `group_topic` producer 并启动 op=4 worker |
| gateway | 2 实例（6666/50055、6667/50075），验证跨网关与 Unknown |
| MySQL | 3306，库 `ape_auth`；新 schema（groups/stream 含 `published_seq`/members/numbers/applications/requests/send_staging/outbox） |
| Mongo | 27017，`IM-System.msg`；**核心用例必需**（不再只是单聊回归依赖） |
| Redis | 6379，密码 `redis123` |
| Kafka | 29092；`msg_topic`（单聊）+ `group_topic`（群，32 分区，消费组 `push-group-fanout`） |
| etcd | 2379；GroupService 注册 `SendGroupMessages`/`GetGroupChatHistory`/`Execute` |
| 工具 | `mariadb`、`mongosh`/`pymongo`、`redis-cli`、`kafka-consumer-groups.sh`、`etcdctl`、`ss`、`pkill` |

### 2.2 启动

建议新增 `scripts/smoke/start_group.sh`（参考 `start_ack.sh`：pkill → 清 etcd → 起服务 → 等端口）：

```bash
pkill -9 -f 'build/main' || true
etcdctl del --prefix /services/ >/dev/null
./build/main --role=backbon --backbon=localhost:50052 &
./build/main --role=auth    --backbon=localhost:50052 &
./build/main --role=group   --backbon=localhost:50052 &
OUTBOX_WORKERS=2 ./build/main --role=msg --backbon=localhost:50052 &
./build/main --role=push    --backbon=localhost:50052 &
GATEWAY_ROUTING=1 ./build/main --role=gateway --backbon=localhost:50052 --ws-port=6666 --push-port=50055 &
GATEWAY_ROUTING=1 ./build/main --role=gateway --backbon=localhost:50052 --ws-port=6667 --push-port=50075 &
```

Python 绑定：`python3 scripts/smoke/gen_proto.py`（产出含新 210/245/246 的 `groupchat_pb2`），
`SMOKE_PYPROTO=/tmp/ape-smoke-pyproto`。

### 2.3 测试开关

| 变量 | 测试值 | 用途 |
|---|---|---|
| `GROUP_INLINE_MAX_MEMBERS` | 200 | 200/201 边界 |
| `GROUP_INLINE_MAX_BYTES` | 262144 | 内联字节边界、同步降级 |
| `GROUP_SYNC_MAX_GROUPS` | 50 | 210 `hasMore` |
| `GROUP_SYNC_MESSAGES_PER_GROUP` | 20 | 小群内联上限 |
| `GROUP_SYNC_MAX_MESSAGES` | 200 | 同步总量上限 |
| `GROUP_SEND_RATE_MEMBER` / `GROUP_SEND_RATE_GROUP` | 默认 5/50；压测 1000 | 限流用例用默认，性能用例放宽 |
| `GROUP_PUBLISH_RETRY_MS` | 1000 | 故障恢复观测 |
| `GROUP_PUBLISH_MAX_ATTEMPTS` | 3（故障用例） | 快速到 tombstone/`state=3` |
| `GROUP_STAGING_RETENTION_HOURS` | 24 | staging 孤儿清理边界（FLT-013） |
| `GROUP_OUTBOX_RETENTION_DAYS` | 7 | outbox 保留期（IDM-007/FLT-013） |
| `GROUP_MEMBERS_CACHE_TTL` | 60 | 缓存失效 |
| `GROUP_OUTBOX_WORKERS` | 2；SND-005/SND-022 用 0 | 提交即返回、stage=0 观测 |

### 2.4 前置检查

- `im_group_messages` **不存在**；`im_groups` 无 `last_seq`；`im_group_members` 无 `ack_seq/read_seq`；
- `im_group_stream` 有 `next_seq/published_seq/epoch/dismissed`；`im_group_send_staging` 存在且 `server_msg_id` 唯一；
- `im_outbox` 有 op=4 行，`payload` 只含引用（`{"serverMsgID":...}`）或空、不含正文；
- `group_topic` 已创建且 `push-group-fanout` 可订阅；
- Mongo 有 `{convID:1,seq:1}`、唯一 `{serverMsgID:1}`（建议加 `{convID,isGroupMsg,seq}`）；
  群文档 `isDeleted` 字段存在（普通消息 `false`，tombstone `true`）；
- 网关已注册 246/245，210 由登录同步直接下发；
- 旧 mock 数据已清理（MySQL 群表 + Mongo `isGroupMsg=true` 文档）。

---

## 3. 测试数据

### 3.1 真实链路

注册 A/B/C（`group_test.register`）→ `91` 登录 → `250 Create` 建群 → B/C `Apply` + owner `Review`。
建群后核验：`stream.next_seq=0`、`stream.published_seq=0`、`epoch=1`、owner `join_seq=0`、
`member_count=1`、群号 10 位。

### 3.2 fixture 群（MySQL 直写）

仅用于 200/201/2000 人、预置 seq、解散/退群等场景；ID 用 18 位 `9000000000000000xx` 段，
账号用 `9000001xx` 段，clientID 前缀 `fx-`，避免与真实数据冲突。

```sql
INSERT INTO im_groups(id,name,owner,member_count,dismissed,created_time,updated_at)
VALUES (900000000000000001,'fixture',900000101,3,0,0,0);
INSERT INTO im_group_stream(group_id,next_seq,published_seq,last_msg_time,epoch,dismissed,updated_at)
VALUES (900000000000000001,100,100,1760000000000,1,0,0);
INSERT INTO im_group_members(group_id,account,active,epoch,join_seq,joined_time,left_time)
VALUES (900000000000000001,900000101,1,1,0,0,0),
       (900000000000000001,900000102,1,1,50,0,0),
       (900000000000000001,900000103,1,1,0,0,0);
```

> 成员表只有 `active/epoch/join_seq`，没有 ack/read；`next_seq` 是**分配水位**、`published_seq` 是
> **可见水位**，fixture 不要另造 `last_seq`。预置历史时通常两者相等；构造“已分配未发布”时只调大
> `next_seq`（SYNC-014）；`published_seq` 不得大于 `next_seq`，构造“已发布但客户端未拉到”时两者相等、由 cursor 控制（PULL-003/006）。

构造未完成 outbox（outbox 引用 + staging 正文，worker 不跑）：

```sql
INSERT INTO im_group_send_staging(server_msg_id,group_id,seq,payload,created_at,updated_at)
VALUES ('fx-stage-101',900000000000000001,101,
        '{"serverMsgID":"fx-stage-101","seq":101,"content":"aGVsbG8="}',0,0);
INSERT INTO im_outbox(operation,dedupe_key,actor_account,peer_account,conversation_id,
                      server_msg_id,seq,payload,stage,state,attempts,created_at,updated_at)
VALUES (4,'g:900000000000000001:900000101:fx-stage-101',900000101,0,
        '900000000000000001','fx-stage-101',101,'{"serverMsgID":"fx-stage-101"}',0,0,0,0,0);
```

> staging/payload 用 MsgData JSON（与实现字段一致）；outbox `payload` 只允许引用，**不得**含正文；
> `stage=0`、`state=0` 表示 worker 尚未写 Mongo。

### 3.3 fixture 历史消息（Mongo 文档）

```python
from bson import Int64
docs = [{
  "serverMsgID": f"fx-{gid}-{seq}", "sendID": sender, "recvID": 0,
  "convID": str(gid), "clientMsgID": f"fx-{seq}", "senderPlatformID": 0,
  "senderNickname": "fx", "senderFaceURL": "", "sessionType": 2, "msgFrom": 100,
  "contentType": 101, "content": b"fixture", "seq": Int64(seq),
  "sendTime": Int64(1760000000000), "status": 1, "isGroupMsg": True,
  "isDeleted": False, "committed": True, "ext": ""
} for seq in seqs]
msg_collection().insert_many(docs)
```

- `convID` 必须是**字符串** groupID，与 245 查询一致；否则查不到；
- 覆盖三种可见性：`committed=true`、`committed=false`、**缺失 `committed` 字段**（视为可见）；
- `isDeleted` 普通消息为 `false`；tombstone 单独构造（见 PULL-019）：

```python
msg_collection().insert_one({
  "serverMsgID": f"hole-{gid}-{seq}", "sendID": 0, "recvID": 0,
  "convID": str(gid), "clientMsgID": f"hole-{seq}", "sessionType": 2,
  "contentType": 101, "content": b"", "seq": Int64(seq),
  "sendTime": Int64(1760000000000), "status": 1, "isGroupMsg": True,
  "isDeleted": True, "committed": True, "ext": ""
})
```

- 跨会话隔离用例插一条 `convID=gid, isGroupMsg=false, seq` 的单聊形态文档；
- 批量 10k 条按 1000/批插入；`seq/sendTime` 超过 int32 时用 `bson.Int64`；
- **不要**插 `im_group_messages`（该表已删除，不在任何新 fixture 中出现）；
- `committed` 仍是唯一可见性字段；`isDeleted=true` 只表示“跳过正文”，不改变可见性（tombstone 也可见）。

### 3.4 清理顺序

```python
def cleanup_group(gid):
    msg_collection().delete_many({"convID": str(gid)})          # 1 Mongo 正文
    sql(f"DELETE FROM im_group_send_staging WHERE group_id={gid}")  # 2 正文暂存
    sql(f"DELETE FROM im_outbox WHERE operation=4 AND conversation_id='{gid}'")
    sql(f"DELETE FROM im_group_members WHERE group_id={gid}")   # 3 子表
    sql(f"DELETE FROM im_group_stream  WHERE group_id={gid}")
    sql(f"DELETE FROM im_group_applications WHERE group_id={gid}")
    sql(f"DELETE FROM im_group_numbers  WHERE group_id={gid}")
    sql(f"DELETE FROM im_groups WHERE id={gid}")                # 4 父表
    redis_delete(f"grp:members:{gid}", f"grp:rate:g:{gid}")
```

- 先 Mongo/staging/outbox，再 members/stream，最后 `im_groups`（有外键）；
- `staging` 也有 `group_id`，既可按群删也可按 `server_msg_id` 精确删；
- `im_group_requests` 按 `(actor, client_id)` 精确删，不按群删；
- 用例 **finally** 清理；失败时保留现场并打印 gid/seq/serverMsgID；
- 复位 `stream.next_seq/published_seq` 或整组删除，避免影响后续用例。

---

## 4. 观测点

| 观测 | 方式 |
|---|---|
| stream | `SELECT next_seq,published_seq,last_msg_time,epoch,dismissed FROM im_group_stream WHERE group_id=?` |
| 成员 | `SELECT account,active,epoch,join_seq FROM im_group_members WHERE group_id=? ORDER BY account` |
| outbox | `SELECT id,state,stage,attempts,server_msg_id,seq,payload FROM im_outbox WHERE operation=4 AND conversation_id=?`（断言 `payload` 只有引用/空，无正文） |
| staging | `SELECT server_msg_id,group_id,seq,payload FROM im_group_send_staging WHERE server_msg_id=?`（按群计数用 `WHERE group_id=?`） |
| 群元数据 | `SELECT id,name,member_count,dismissed FROM im_groups WHERE id=?` |
| 成员请求幂等 | `SELECT actor,client_id,payload,reply FROM im_group_requests WHERE actor=? AND client_id=?` |
| Mongo 计数 | `countDocuments({convID:gid,isGroupMsg:true})`、`committed:false`、`committed:{$ne:false}`、`isDeleted:true` |
| Mongo 序列 | `find({convID:gid}).sort({seq:1})` 取 `seq/serverMsgID/committed/isDeleted`，核对 `min/max` 与连续性 |
| Mongo 单文档 | 取一条断言 `recvID=0/sessionType=2/isGroupMsg=true/sendID/seq/sendTime/isDeleted` |
| Redis | `HGETALL grp:members:{gid}`、`GET grp:rate:m:{gid}:{uid}`、`GET gw:user:{account}` |
| Kafka | `kafka-consumer-groups.sh --bootstrap-server localhost:29092 --describe --group push-group-fanout` |
| WS 帧 | 收集 `246/245/251/210`；245 解析 `lastSeq/deletedSeq`；核对**没有**群字段的 `209`、没有 ACK 帧、250 只回成员命令 |
| 指标 | outbox 积压/发布失败/tombstone 数/fanout 延迟/Unknown 比例/seq 等待（命名以实现为准） |
| 日志 | `grep 'groupID=...' /tmp/ape-smoke-*.log`，确认 traceID/seq/serverMsgID 可关联 |

Mongo 可见性查询固定为 `{convID, isGroupMsg:true, committed:{$ne:false}, seq:{$gt:from,$lte:to}}`；
缺失 `committed` 视为可见，`committed=false` 必须过滤；tombstone（`isDeleted=true`）不能被该过滤误删。

---

## 5. 断言语义

- **精确值**：`stream.next_seq`、`stream.published_seq`、Mongo 文档数、outbox 行数/`state`/`stage`、
  staging 行数、`member_count`；
- **两个水位**：`next_seq` = 已分配最大 seq；`published_seq` = 已发布（Mongo `committed=true`，含
  tombstone）最大 seq；`Group.lastSeq`、210 `lastSeq`、245 `lastSeq`/上界都断言 `published_seq`，
  不是 `next_seq`；
- **needsPull**：`needsPull = (toSeq < lastSeq)`；内联未覆盖 `published_seq` 时必须 Pull（SYNC-004/014）；
- **集合**：接收方 `serverMsgID` 集合（去重后）与发送方 `seq` 集合；
- **连续**：seq 分配严格 `1..N`、无分配空洞（回滚的 seq 可复用）；永久空洞是有 tombstone 文档的 seq
  （PULL-019），不是 seq 跳号；
- **可见性**：246 成功 ≠ 立即可见；只有 `committed != false` 才允许出现在 245/同步 251；
  `isDeleted=true` 也返回，但客户端跳过正文；
- **staging**：发送事务提交后 staging 1 行（含正文）、outbox 1 行（payload 仅引用）；worker 写 Mongo
  后删除 staging；staging 与 outbox 同生共死，不允许“有 outbox 无正文”；
- **tombstone**：outbox 放弃时 `isDeleted=true`、`committed=true`、无正文；245 返回它并带
  `deletedSeq=本页最大 tombstone seq`；客户端跳过正文但推进 cursor；
- **时序**：`wait_for(predicate, timeout)` 轮询，不用固定 sleep 断言；推送允许乱序/重复；
- **帧**：251 的 `groupID/lastSeq/epoch/messages`，210 的 `groups/messagesPushed/hasMore`，
  245 的 `messages/hasMore/nextSeq/minSeq/lastSeq/epoch/deletedSeq`；
- **顺序**：登录时 `209`（单聊）→ 群 `251`（内联）→ `210`；`210` 是群同步最后一帧；
- **连接**：246/245 业务错误只回错误码，不断连，随后 ping/pong 正常；
- **错误码**：以 `SendGroupMessageResp.errorCode` / `GetGroupChatHistory` 外层错误码 /
  `Reply.errorCode` 为准，测试同时记录外层 `SdkWSResp.errorCode`。

---

## 6. 设计方法

| 方法 | 应用 |
|---|---|
| 等价类 | 成员/非成员、正常/解散、text/非 text、合法/非法 groupID/clientID |
| 边界值 | 64KiB、200/201/2000 人、limit 0/1/100/101、50 群、20 条/群、200 条总量、staging 24h |
| 状态迁移 | outbox state 0→1→2/3、stage 0→1、`published_seq` 推进、成员 active 1→0→1、群正常→解散 |
| 并发/竞态 | 同 clientID、同群并发、发送 vs 踢人/解散/入群、并发审批/退群 |
| 故障注入 | 停/挂起 MySQL/Mongo/Kafka/Redis/Push/Group/Gateway，杀进程，慢依赖，outbox 超过最大尝试 |
| 一致性 | stream 的 `next_seq/published_seq` 与 Mongo 的 seq/数量、outbox 与 staging/Mongo 的正文 |
| 性能 | 吞吐、p99、Kafka lag、锁等待、长稳 |

---

## 7. 用例总览（核心 137 条）

> 137 = SND 22 + IDM 9 + FAN 16 + PULL 20 + SYNC 14 + MEM 18 + FLT 15 + PERF 8 + REG 8 + SEC 7。


### 7.1 发送（SND）

| ID | 优先级 | 场景 | 期望摘要 |
|---|---|---|---|
| SND-001 | P0 | 小群单条发送（246） | 响应 0；`seq=1`；`next_seq=published_seq=1`；Mongo 1 doc；staging 已删；outbox payload 无正文、state=2；B 收 251 内联 |
| SND-002 | P0 | 连续发送 100 条 | `seq=1..100` 连续；`next_seq=100`、最终 `published_seq=100`；Mongo 100 doc |
| SND-003 | P0 | 20 连接并发 200 条（放宽限流） | 全部成功；seq 集合={1..200}；无死锁 |
| SND-004 | P0 | 两个 GroupService 实例并发 | seq 唯一连续；stream 行锁不重号 |
| SND-005 | P0 | 提交即返回、不等 Mongo | worker 暂停时 246 仍 0；outbox stage=0 且 payload 仅引用；staging 有正文；恢复后 Mongo `committed=true`、staging 删除、`published_seq=seq` |
| SND-006 | P1 | 内容 65536 / 65537 字节 | 前者成功；后者 `MESSAGE_TOO_LARGE`；`next_seq` 不变 |
| SND-007 | P1 | 空/纯空白 content | `EMPTY_MESSAGE`；无 staging/outbox/Mongo |
| SND-008 | P0 | 非成员 / 未知群 | `NOT_MEMBER` / `GROUP_NOT_FOUND`；无 seq |
| SND-009 | P0 | 已退群（active=0）发送 | `NOT_MEMBER` |
| SND-010 | P0 | 已解散群发送 | `GROUP_DISMISSED` |
| SND-011 | P1 | 非法 groupID（abc/-1/0/21 位） | `INVALID_GROUP`；连接保持 |
| SND-012 | P1 | clientID 空/65 字节/含空格 | `INVALID_REQUEST_ID`；不消耗 seq |
| SND-013 | P1 | 成员 6 条/秒 | 第 6 条 `RATE_LIMITED`；仅 5 个 seq |
| SND-014 | P1 | 全群 51 条/秒（多成员） | 超限 `RATE_LIMITED`；已成功的不回滚 |
| SND-015 | P1 | 注入 outbox INSERT 失败 | `DB_ERROR`；`next_seq` 不变；无 staging、无 outbox、无 Mongo doc |
| SND-016 | P0 | 发送 vs 踢人竞态 | 二者串行；被踢后返回 `NOT_MEMBER`；无 outbox |
| SND-017 | P0 | 发送 vs 解散竞态（两方向） | 已提交消息保留；解散后新发送 `GROUP_DISMISSED`；无死锁；staging/outbox 同回滚 |
| SND-018 | P0 | 发送 vs 审批入群（两顺序） | `joinSeq=入群时 next_seq`；只可见 `seq>joinSeq` |
| SND-019 | P1 | contentType=0 / 102 | 0 视为 101 成功；102 `INVALID_PARAM` |
| SND-020 | P2 | 64KiB 多字节 UTF-8 | 不截断字符；边界语义与 SND-006 一致 |
| SND-021 | P1 | 2000 人群发送 | 成功；251 hint 无正文；245 可拉；`published_seq` 最终推进 |
| SND-022 | P1 | staging 与 outbox 同事务、正文不丢 | 提交后 staging 有正文、outbox payload 仅引用；staging 失败则两者皆无；worker 从 staging 读正文且 Mongo 正文一致 |

### 7.2 幂等（IDM）

| ID | 优先级 | 场景 | 期望摘要 |
|---|---|---|---|
| IDM-001 | P0 | 同 (group,sender,clientID) 同内容重发 | 同 `seq/serverMsgID`；Mongo/outbox 不增；staging 不重复；`next_seq` 只 +1 |
| IDM-002 | P0 | 同 clientID 不同内容 | `IDEMPOTENCY_CONFLICT`；不新增 |
| IDM-003 | P1 | 同 clientID 不同群 | 各自成功；两条消息 |
| IDM-004 | P1 | 同 clientID 不同发送者 | 各自成功；互不覆盖 |
| IDM-005 | P0 | 10 并发同 clientID | 全部 0；同一 seq/serverMsgID；1 doc/1 outbox；seq 无空洞；staging 最终 0 |
| IDM-006 | P1 | 提交后断连/响应丢失，重连重发 | 命中幂等，同一 seq |
| IDM-007 | P2 | outbox 7 天保留期边界 | 保留期内命中；清理后同 clientID 视为新消息；staging 24h 清理不影响（正文已在 Mongo） |
| IDM-008 | P1 | 成员命令 clientID 重放 | 返回原 `Reply`；Create/Apply/Review/Kick 只 1 次效果 |
| IDM-009 | P1 | stage=0 时重发（outbox 已写、正文在 staging、Mongo 未写） | 返回同一消息；从 staging 读正文（不是 outbox payload）；worker 只写 1 doc |

### 7.3 扇出与推送（FAN）

| ID | 优先级 | 场景 | 期望摘要 |
|---|---|---|---|
| FAN-001 | P0 | 小群在线成员 | 1s 内收 251，含 1 条 MsgData；发送者不收 |
| FAN-002 | P1 | 发送者本人 | 251 收件人不含 sender |
| FAN-003 | P0 | 离线成员 | 不推送；登录后 210+245 补齐 |
| FAN-004 | P0 | 跨两个网关 | 各网关成员都收 251；`gw:user` 路由正确 |
| FAN-005 | P1 | 200 active（发送者在内） | 251 含正文；单帧 ≤256KiB |
| FAN-006 | P1 | 201 active 大群 | 251 只带 `lastSeq=published_seq`/epoch；245 可拉 |
| FAN-007 | P1 | 成员缓存 miss | 回源 SQL(active=1) 后正确扇出 |
| FAN-008 | P1 | 缓存含已退群成员 | 最多多推 1 帧；245 拒绝该成员 |
| FAN-009 | P1 | 新成员 joinSeq 过滤 | 不收 `seq<=joinSeq` 的消息 |
| FAN-010 | P1 | 推送乱序/重复 | 客户端按 seq 排序、serverMsgID 去重 |
| FAN-011 | P1 | Kafka 重放同一条事件 | 至多重复 251；Mongo 只 1 doc |
| FAN-012 | P0 | 一网关 Unknown | 不重试、不阻塞分区；其他群 1s 内正常 |
| FAN-013 | P1 | 成员无路由 | 记 NotOnline，不向所有网关广播 |
| FAN-014 | P1 | PushService 重启 | 位点续跑/重放；消息可 245 |
| FAN-015 | P1 | 慢客户端写队列满 | 断连；重连后 245 补齐 |
| FAN-016 | P1 | 内联超字节（调低上限/同步多消息） | 降级 hint；245 可拉；指标 mode=hint |

### 7.4 拉取（PULL）

| ID | 优先级 | 场景 | 期望摘要 |
|---|---|---|---|
| PULL-001 | P0 | 基础拉取 | 升序、≤limit、`hasMore`/`nextSeq` 正确 |
| PULL-002 | P0 | after < joinSeq | `from=joinSeq`；`minSeq=joinSeq`；不返回旧消息 |
| PULL-003 | P1 | snapshot 固定上界 | 不被新消息追上；`lastSeq=min(snapshot,published_seq)` |
| PULL-004 | P0 | 分页 1..N | 不丢不重；页间 seq 单调 |
| PULL-005 | P1 | limit 0/-1/101 | 0→20；101→100；负数拒绝（错误码实现固定） |
| PULL-006 | P1 | after > lastSeq | 空结果；`nextSeq=from`；`lastSeq=min(snapshot,published_seq)` |
| PULL-007 | P0 | 非成员 | `NOT_MEMBER`；无消息 |
| PULL-008 | P1 | 已解散 | `GROUP_DISMISSED` |
| PULL-009 | P1 | 未知群 | `GROUP_NOT_FOUND` |
| PULL-010 | P1 | 空群 | 空；`lastSeq=published_seq=0`；`minSeq=joinSeq` |
| PULL-011 | P1 | 字段完整性 | `minSeq/lastSeq(=published_seq)/epoch` 与 stream/member 一致 |
| PULL-012 | P0 | committed 过滤 | `false` 不返回；`true`/缺失返回；tombstone 因 `committed=true` 返回 |
| PULL-013 | P1 | 与发送并发 | 按 snapshot 一致；无重复；缺口可重拉 |
| PULL-014 | P1 | 跨会话隔离 | 同 seq 的 `isGroupMsg=false` 文档不出现 |
| PULL-015 | P0 | 最新 N（beforeSeq=lastSeq+1） | 返回最近 limit 条，升序；上界=`published_seq`；`hasMore` 表示还有更旧 |
| PULL-016 | P0 | beforeSeq 翻旧 | 返回 beforeSeq 之前 limit 条；`nextSeq=最早 seq`；上界不超过 `published_seq`；可继续 |
| PULL-017 | P1 | beforeSeq<=joinSeq | 空；`nextSeq=minSeq`；`hasMore=false` |
| PULL-018 | P1 | after/before 同时>0 | `INVALID_PARAM` |
| PULL-019 | P0 | tombstone 与 deletedSeq | 245 返回 `isDeleted=true` 的 MsgData；`deletedSeq=本页最大 tombstone seq`；客户端跳过正文但 cursor 推进；后续消息正常 |
| PULL-020 | P1 | 前台 30s 对账补漏 | 丢弃 251 后，30s 内主动 `245{afterSeq=cursor,limit=20}` 能补齐；cursor 只按连续消息推进 |

### 7.5 登录同步（SYNC）

| ID | 优先级 | 场景 | 期望摘要 |
|---|---|---|---|
| SYNC-001 | P0 | 209 后 251+210 | 顺序 209→251→210；210 含 range 字段 |
| SYNC-002 | P1 | 无群/空同步 | 210 仍发；`groups=[]`、`messagesPushed=0` |
| SYNC-003 | P1 | 小群待补 ≤20 | 一帧 251 含 ≤20 条已发布 MsgData（含 tombstone） |
| SYNC-004 | P0 | 大群待补 | 不发 251；`lastSeq=published_seq`；`needsPull=(toSeq<lastSeq)=true`；245 补齐 |
| SYNC-005 | P0 | needsPull 后 245 | 分页补齐，不丢不重 |
| SYNC-006 | P1 | >50 个 active 群 | 210 `hasMore=true`；按 lastMsgTime 取 50 |
| SYNC-007 | P1 | 总量/字节上限 | 总消息≤200；每群内联按 256KiB 截断，0 条装下则 needsPull |
| SYNC-008 | P1 | 客户端 cursor ≥ published_seq | 跳过该群；`cursor<published_seq` 不跳过 |
| SYNC-009 | P1 | 单群查询失败 | 210 `PARTIAL`；其他群正常 |
| SYNC-010 | P1 | 已解散群 | 不入 ranges；不推 251 |
| SYNC-011 | P1 | 无 cursor | 从 `joinSeq` 起 |
| SYNC-012 | P1 | 同步与发送并发 | 以 `published_seq` snapshot 为准；不漏已发布消息 |
| SYNC-013 | P1 | 209 无群字段 | `SyncSummary` 不含 group 字段；群数据只在 210 |
| SYNC-014 | P0 | published_seq 落后/内联未覆盖 | `lastSeq=published_seq`（不是 next_seq）；未发布 seq 不内联/不返回；内联 `toSeq<lastSeq` 时 `needsPull=true`，由 245 补齐；`lastSeq` 不得当已拉取 |

### 7.6 成员生命周期（MEM）

| ID | 优先级 | 场景 | 期望摘要 |
|---|---|---|---|
| MEM-001 | P0 | 创建群 | groups/stream/群号/owner 成员；`next_seq=published_seq=0`、`epoch=1`、count=1 |
| MEM-002 | P1 | 重复申请 | 只 1 条 pending；重复 Apply 返回 0 |
| MEM-003 | P0 | 审批通过 | `join_seq=当时 next_seq`；`epoch+1`；count+1；active=1 |
| MEM-004 | P1 | 审批拒绝 | 申请人 `MyApplications` status=2；不入群 |
| MEM-005 | P1 | 重复审批 | `APPLICATION_HANDLED`；无二次入群 |
| MEM-006 | P1 | 非 owner 管理 | Review/Rename/Kick/Dismiss → `OWNER_REQUIRED` |
| MEM-007 | P0 | 退群 | active=0；`epoch+1`；count-1；245 拒绝 |
| MEM-008 | P1 | 群主退群 | `OWNER_CANNOT_LEAVE` |
| MEM-009 | P0 | 踢人 + 重复踢 | 目标 active=0；成员数-1；重复踢幂等 |
| MEM-010 | P0 | 重入 | `epoch+1`；`join_seq=当时 next_seq`；旧消息不可见 |
| MEM-011 | P0 | 解散 | stream dismissed=1；`epoch+1`；关 pending；发送/拉取拒绝 |
| MEM-012 | P1 | 解散幂等/竞态 | 重复解散不报错；无新消息/新成员；通知只发一次 |
| MEM-013 | P1 | 改名 | owner 生效；251 refresh；非 owner 拒绝 |
| MEM-014 | P1 | 200 人分页 | limit 17 共 12 页；不丢不重；只含 active |
| MEM-015 | P1 | 群满 2000 | 第 2000 人成功；第 2001 人 `GROUP_FULL` |
| MEM-016 | P1 | 群号 | 10 位唯一；按群号/群 ID 搜索 identity 不变；回填幂等 |
| MEM-017 | P1 | 旧 epoch 操作 | `MEMBERSHIP_CHANGED`；无副作用 |
| MEM-018 | P1 | 并发审批/退群/踢人 | 只生效一次；`member_count` 不漂移 |

### 7.7 故障（FLT）

| ID | 优先级 | 场景 | 期望摘要 |
|---|---|---|---|
| FLT-001 | P0 | MySQL 不可用 | 246 `DB_ERROR`；无 staging/outbox/seq；245 失败；连接保持 |
| FLT-002 | P0 | 提交后响应丢失 | 重连重发同 clientID → 同 seq，不重复 |
| FLT-003 | P0 | Kafka 不可用 | 发送成功；Mongo 可见；`published_seq` 已推进；无 251；outbox stage=1/state=0 重试；恢复补推 |
| FLT-004 | P0 | GroupService 重启 | op=4 续跑；同 serverMsgID 只 1 doc；不产生不可见洞（放弃写则 tombstone） |
| FLT-005 | P1 | PushService 重启 | 重平衡/重放；可 245；客户端去重 |
| FLT-006 | P0 | 网关崩溃 | 重连 210/245 补齐；路由切换 |
| FLT-007 | P1 | Redis 不可用 | 限流放行；成员回源；无路由→NotOnline 不广播 |
| FLT-008 | P1 | 扇出中崩溃 | 可能漏推/重复；Kafka 重投或 245 兜底 |
| FLT-009 | P0 | Mongo 不可用 | 发送成功；stage=0、staging 保留；`published_seq` 不推进；245 暂无该 seq；恢复后可见并推进 |
| FLT-010 | P1 | Mongo 达最大尝试 | Mongo 恢复后写 tombstone（`isDeleted=true,committed=true`）；`published_seq` 推进；state=3；245 返回 `isDeleted`/`deletedSeq`；客户端可跳过；tombstone 写失败继续重试 |
| FLT-011 | P1 | Kafka 达最大尝试 | 已 `committed=true`、`published_seq` 已推进；state=3 仍可 245；**无 tombstone**；只缺推送 |
| FLT-012 | P1 | 磁盘满/表只读 | 246 `DB_ERROR`；无半写；`next_seq` 不变 |
| FLT-013 | P1 | outbox/staging 保留期清理 | state=2/3 超 7 天删 outbox；state=0/1 永不删；staging stage=1 后删、孤儿超 24h 清；不误删单聊 op |
| FLT-014 | P1 | 依赖慢 5s | 246 不因 Mongo/Kafka 慢而超时；worker 重试；无重复 |
| FLT-015 | P1 | Kafka 事件损坏 | 跳过并告警；不阻塞分区；lag 可回落 |

### 7.8 性能（PERF）

| ID | 优先级 | 场景 | 期望摘要 |
|---|---|---|---|
| PERF-001 | P0 | 单群 1000 msg/s ×60s（放宽限流） | 无错/无分配洞；246 p99<50ms；outbox 最终清空；tombstone=0 |
| PERF-002 | P1 | 2000 人 50 msg/s，50 在线 | hint+245；在线 p99<1s；网关/MySQL 稳定 |
| PERF-003 | P1 | 10k 群 ×10 msg/s | 总吞吐线性；无热分区；lag 可回落 |
| PERF-004 | P1 | 登录同步 50 群 + 50 单聊 | <3s；210 `hasMore` 正确；写队列不溢出 |
| PERF-005 | P2 | 245 拉 100×64KiB | 5s 内完成；内存有界 |
| PERF-006 | P2 | 10k 条历史分页 | 每页 100；延迟可控；无重复 |
| PERF-007 | P1 | 成员缓存命中率 | 稳定态 >90%；miss 回源可接受 |
| PERF-008 | P1 | 24h 长稳 | 无泄漏；outbox/request/staging 有界；lag 不持续增长 |

### 7.9 回归/安全（REG/SEC）

| ID | 优先级 | 场景 | 期望摘要 |
|---|---|---|---|
| REG-001 | P0 | 单聊 `ack_test.py`/`seq_test.py` | 全部通过 |
| REG-002 | P0 | 250 成员/元数据命令 | 上述命令语义不变；响应去 unread/readSeq，新增 `joinSeq/lastSeq(=published_seq)/epoch/large` |
| REG-003 | P0 | 旧 250 Send/Ack/Read/Pull | 均返回 `UNSUPPORTED_OPERATION`；连接保持 |
| REG-004 | P1 | 209 单聊语义 | 无群字段；群同步只走 210 |
| REG-005 | P1 | 244 单聊拉取 | 不受群链路影响；245 独立 |
| REG-006 | P1 | 单聊 101/104/106 | 不变；群不发 106，也没有 ACK 帧；106 带群 ID 仍非法 |
| REG-007 | P1 | schema 清理 | 无 `im_group_messages`；`im_group_stream` 有 `published_seq`；`im_group_send_staging` 存在；groups 无 last_seq；members 无 ack/read |
| REG-008 | P1 | 服务注册/初始化 | etcd 可见 3 个方法；网关路由 246/245；`InitializeGroups` 幂等 |
| SEC-001 | P0 | 伪造 actor | 246.actor/245.ownerUserID 被网关覆盖；不能替他人发送/读取 |
| SEC-002 | P0 | 非成员读历史 | 245 `NOT_MEMBER`；无数据泄露 |
| SEC-003 | P0 | 非 owner 管理 | `OWNER_REQUIRED` |
| SEC-004 | P1 | 超大 WS 帧（>1MiB） | 拒绝/断开，不 OOM；重连正常 |
| SEC-005 | P1 | groupID SQL 注入 | 参数化；`INVALID_GROUP`；表无损 |
| SEC-006 | P1 | 刷屏 | 成员/群限流生效；服务稳定 |
| SEC-007 | P1 | 退群/被踢后访问 | 246/245 均 `NOT_MEMBER` |

---

## 8. 关键用例详解

### 8.1 SND-001 小群单条发送（246）

**前置**：A（owner）、B 成员；`next_seq=0`、`published_seq=0`；A 在 gw-1、B 在 gw-2；B 在线。

**步骤**

1. A 发 `246 SendGroupMessageReq{groupID=G, clientID="c1", content="hello", contentType=101}`；
2. 解析 `SendGroupMessageResp`，记录 `serverMsgID/seq/sendTime`；
3. 立即查 staging/outbox（worker 未完成时），再 `wait_for` outbox state=2 与 `published_seq=1`，查 stream/Mongo；
4. B 收 251；A 的连接不收到该 251。

**期望**

- 响应 0；`msg.seq=1`、`msg.serverMsgID` 非空、`isGroupMsg=true`、`recvID=0`、`sessionType=2`；
- `stream.next_seq=1`、`stream.published_seq=1`、`last_msg_time>0`；
- staging：worker 完成前 1 行且含正文，完成后 0 行；
- outbox 1 行：`operation=4`、`stage=1`、`state=2`、`server_msg_id` 与响应一致；`payload` 只含
  `serverMsgID` 引用、**不含正文**；
- Mongo 1 doc：`convID=G`、`seq=1`、`committed=true`、`isDeleted=false`、`isGroupMsg=true`；
- B 的 251：`messages[0].seq=1`、`lastSeq=1`、`epoch` 与 stream 一致；
- `seq=1..` 无第二条文档；A 无自己的 251。

### 8.2 SND-017 发送与解散竞态

**前置**：owner A、成员 B；群正常；准备两个方向。

**方向 A（发送先持 stream）**

1. 事务 T1：锁 B 成员行 → `UPDATE im_group_stream SET next_seq=next_seq+1 ... WHERE dismissed=0`，不提交；
2. A 并发 Dismiss：应阻塞在 stream 行锁；
3. T1 提交；Dismiss 继续并提交；
4. 观察 B 的消息与后续发送。

**方向 B（解散先持 stream）**

1. 事务 T2：`UPDATE im_group_stream SET dismissed=1, epoch=epoch+1`，不提交；
2. 并发发送：锁成员行后在 stream `UPDATE ... WHERE dismissed=0` 等待；
3. T2 提交；发送的 UPDATE 应 affected=0。

**期望**

- 方向 A：`seq=N` 的消息保留（解散前已提交）；Dismiss 后新发送 `GROUP_DISMISSED`；
- 方向 B：发送 `GROUP_DISMISSED`；无 staging/outbox/Mongo 新文档；
- 两方向均无死锁；`next_seq` 不因失败回退，也不留分配空洞。

### 8.3 IDM-005 10 并发同 clientID

**前置**：A 成员；`next_seq=0`；关闭/放宽成员限流。

**步骤**

1. 10 个并发 246，同 `clientID="dup-1"`、同内容；
2. 等全部响应；查 stream/outbox/staging/Mongo。

**期望**

- 10 个响应均 0，`seq/serverMsgID` 完全相同；
- `next_seq=1`；outbox 1 行；Mongo 1 doc；
- 无 seq 空洞（唯一键冲突的事务整体回滚，含 seq 自增与 staging）；
- 若有响应超时，重试仍命中同一消息。

### 8.4 FAN-012 网关 Unknown 不阻塞、不广播

**前置**：测试 topic 设为 1 分区（或选同分区 G1/G2）；G1 成员在 gw-2，使 gw-2 的 push 端口不可达
（`iptables DROP 50075` 或停 gw-2 的 push server）；G2 成员在 gw-1。

**步骤**

1. 向 G1 发一条 → 扇出对 gw-2 得 Unknown；
2. 立即向 G2 发一条；
3. 观察 G2 的 251、Kafka lag、日志/metric；
4. 恢复 gw-2；G1 成员主动 245。

**期望**

- Unknown 只记 metric/日志，不重试、不阻塞；G2 在 1s 内收到；
- `push-group-fanout` lag 不持续增长；
- 不给离线成员/所有网关广播（RPC 数 ≈ 在线账号/500×实例数）；
- G1 成员恢复后 245 能拉到，消息不丢。

### 8.5 PULL-004/012 245 分页与 committed 过滤

**前置**：fixture 群 G，`joinSeq=0`，`next_seq=100`、`published_seq=100`；Mongo 预置 seq 1..100：
seq 50 `committed=false`，seq 51 缺失（模拟 worker 未写），seq 52 **无 `committed` 字段**，其余 `true`；
另插一条 `convID=G,isGroupMsg=false,seq=60`。

> `published_seq` 是“最大已发布”，不保证 ≤ 它的每条都已发布（ADR-14）；因此允许 50/51 缺口存在。

**步骤**

1. `245{afterSeq=0,limit=20}`，循环 `nextSeq` 至 `hasMore=false`；
2. 汇总 seq/serverMsgID；
3. 把 seq 50 改为 `committed=true`，插入 seq 51 `committed=true`；
4. 从 `afterSeq=49` 重拉。

**期望**

- 首次汇总 = `{1..49,52..100}`（50/51 不可见，52 可见）；
- 不含 `isGroupMsg=false` 的 60；无重复；`minSeq=0`、`lastSeq=100=published_seq`、`epoch` 正确；
- `nextSeq` 为本页最后一条 seq；空页回显 `from`（语义见 §13）；
- 第二次从 49 拉得到 50、51、52；客户端补齐后 cursor 才推进。

### 8.6 SYNC-004 大群 210 needsPull

**前置**：B 在群 G（201 active，`next_seq=30`、`published_seq=30`，`joinSeq=0`，`epoch=2`），
cursor=0；B 离线期间 1..30 已发布。

**步骤**

1. B `91` 登录，收集 209 后的帧直到 210；
2. 解析 `GroupSyncSummary`；
3. 对 range 调 245：先 `beforeSeq=31,limit=20`，再 `beforeSeq=11,limit=20`。

**期望**

- 209 只含单聊；G 不发 251 内联；
- 210 有且仅有一个 G range：`groupID=G`、`fromSeq=1`、`lastSeq=30`、`joinSeq=0`、`epoch=2`、
  `needsPull=true`、`hasGap=false`（`fromSeq=max(cursor,joinSeq)+1`，`cursor=joinSeq=0`）；
  大群不内联，`toSeq=0`（无实际内联区间），`needsPull=(0<30)=true`；
- `messagesPushed=0`、`hasMore=false`；
- 第一次 245 返回 11..30（升序，`hasMore=true`，`nextSeq=11`）；第二次返回 1..10；
- 两页合并 1..30，不丢不重；210 后连接正常。

### 8.7 FLT-003/011 Kafka 不可用与最大尝试

**前置**：停 Kafka；`GROUP_PUBLISH_MAX_ATTEMPTS=3`、`GROUP_PUBLISH_RETRY_MS=1000`。

**步骤**

1. A 发 1 条；查 246 响应、Mongo、stream、outbox；
2. B 不期待 251；B 主动 245；
3. 等 outbox 达最大尝试，查 state/stage/Mongo committed/`published_seq`/是否有 tombstone；
4. 恢复 Kafka，等 state=2；B 收补推或已拉取。

**期望**

- 246 成功；Mongo 有 doc 且 `committed=true`（`PublishMessage` 先于 Kafka produce）；
- `published_seq` 已推进到该 seq（发布成功即推进，与 Kafka 无关）；
- outbox：`stage=1`、`state=0` 重试；达上限后 `state=3` 并告警；
- 245 能拉到该消息（`committed=true`）；B 无实时 251；
- **无 tombstone**（消息正文已落 Mongo）；恢复后 outbox 可继续发布到 state=2（或已 state=3 则只告警，
  不重复写 Mongo）。

### 8.8 MEM-010 重入与可见性

**前置**：G 已有 seq 1..5；B 在 `next_seq=5` 时入群（`joinSeq=5`、epoch=2）；B 发 seq 6；B 退群
（epoch=3）；A 发 seq 7；B 重新审批入群（此时 `next_seq=7`）。

**步骤**

1. 第一次入群后 B 245：只看到 6；
2. 退群后 B 246/245：`NOT_MEMBER`；
3. 重入后查成员行，B 245 `afterSeq=0`；
4. B 发 seq 8，查 stream/Mongo。

**期望**

- 重入后 `epoch=4`、`join_seq=7`、active=1、count 恢复；
- B 245 只返回 8（旧 1..7 不可见），`minSeq=7`；
- 退群期间 246/245 均 `NOT_MEMBER`；
- 缓存 `grp:members:{G}` 已被 DEL，扇出不推旧成员。

---

### 8.9 PULL-015/016 最新 N 与翻旧

**前置**：G 有 seq 1..100，B 的 joinSeq=0；全部 `committed=true`、`published_seq=100`。

**步骤**

1. B 调 `245{beforeSeq=101,limit=20}`（lastSeq=100，latest=beforeSeq=lastSeq+1）；
2. 再调 `245{beforeSeq=<上一步最早 seq>,limit=20}`；
3. 继续直到 `hasMore=false`；
4. 另测 `245{afterSeq=50,beforeSeq=100}`。

**期望**

- 第一次返回 81..100，升序，`hasMore=true`，`nextSeq=81`；
- 第二次返回 61..80，`nextSeq=61`；
- 能翻到 1..20 且无重复；`minSeq=0`、`lastSeq=100=published_seq`；
- 同时带 after/before → `INVALID_PARAM`，不返回消息。

### 8.10 SND-022 staging 与 outbox 同事务、正文不丢

**前置**：A 成员；`next_seq=0`、`published_seq=0`；`GROUP_OUTBOX_WORKERS=0`（worker 不跑）。

**步骤**

1. A 发 246（clientID=`stage-1`，content=`hello`）；
2. 查 `im_group_send_staging WHERE server_msg_id=?` 与 `im_outbox.payload`；
3. 确认 Mongo 无该 serverMsgID 文档（worker 未写），启动 worker；
4. 等 outbox state=2 后查 Mongo/staging/stream；
5. 另一次发送注入 staging INSERT 失败（临时触发器），观察事务回滚。

**期望**

- 响应 0；staging 1 行，`group_id/seq` 与响应一致，payload 含完整 MsgData 正文；
- outbox 1 行：`stage=0`、`payload` 只有 `serverMsgID` 引用（无 content）；
- worker 从 staging 取正文写 Mongo，Mongo `content` 与发送一致；stage=1 后 staging 删除、
  `published_seq=seq`；
- staging 失败的事务：无 staging、无 outbox、`next_seq` 不变、无 Mongo doc（同 SND-015 的半写断言）。

### 8.11 PULL-019 tombstone 与 deletedSeq

**前置**：fixture G，`joinSeq=0`、`published_seq=10`；Mongo seq 1..10，其中 seq 5 为 tombstone
（`serverMsgID="hole-G-5"`、`isDeleted=true`、`committed=true`、`content=b""`），其余 `isDeleted=false`。

**步骤**

1. B `245{afterSeq=0,limit=20}`；
2. 逐条处理：`isDeleted` 跳过正文、该 seq 计入已处理；
3. 再 `245{afterSeq=5,limit=20}`；
4. 查 Mongo/stream。

**期望**

- 响应 0；`messages` 含 seq 5，`isDeleted=true`、`committed=true`、content 为空；
- `deletedSeq=5`；其余消息 `isDeleted=false`；
- 客户端 cursor 推进到 10，不会卡在 4→6；后续 245 返回 6..10（升序）；
- `lastSeq=10=published_seq`；若 251 内联含 tombstone，客户端同样跳过正文并推进 cursor；
- 其他成员/后续消息不受影响，tombstone 不阻塞 Pull。

### 8.12 PULL-020 前台 30s 对账补漏

**前置**：B 已加载 1..10（cursor=10）；服务端 `published_seq=20`；B 在线且前台打开该群；
B 的 251 通道被丢弃（`kill -STOP` push 或测试客户端丢弃 251）。

**步骤**

1. A 发 11..20（或 fixture 已发布 11..20）；
2. 确认 B 未收到 11..20 的 251；
3. 等 30s 对账定时器触发（测试可直接调用 `245{afterSeq=10,limit=20}` 模拟）；
4. B 处理 245 响应并推进 cursor；恢复 251。

**期望**

- 30s 周期内（允许 ≤35s）B 主动 245 拉到 11..20，升序；`hasMore=false`；
- cursor 从 10 连续推进到 20；不因 `lastSeq=20` 直接跳；
- 若响应被 limit 截断（如只到 15），cursor 只到 15，下个周期继续；
- 重复 251/245 不产生重复渲染（serverMsgID 去重）。

### 8.13 FLT-010 Mongo 超最大尝试写 tombstone

**前置**：A 成员；`GROUP_PUBLISH_MAX_ATTEMPTS=3`、`GROUP_PUBLISH_RETRY_MS=1000`；
`next_seq=published_seq=10`；Mongo 暂停（`docker pause mongo-standalone`）。

**步骤**

1. A 发 1 条（seq=11）；等 worker 尝试超过最大次数；
2. 查 outbox `state/stage/attempts`、staging、`published_seq`、Mongo（应无 doc）；
3. 恢复 Mongo（`unpause`）；等 worker 写 tombstone；
4. 查 Mongo doc/stream/outbox/staging；
5. B 主动 `245{afterSeq=10,limit=20}`；A 再发 seq=12，B 再 245。

**期望**

- Mongo 不可用期间：246 已成功；outbox `stage=0`、`state=0`、attempts 增长；**不 state=3**、
  `published_seq` 不推进、staging 保留（不能留不可见空洞）；
- 恢复后：Mongo 有 1 doc `seq=11`、`isDeleted=true`、`committed=true`、content 空；
  `published_seq=11`；outbox `state=3`；staging 删除；
- B 的 245 含 seq=11，`isDeleted=true`、`deletedSeq=11`；客户端跳过正文并推进 cursor；
- 后续 seq=12 正常发送/拉取，不被 tombstone 阻塞。

---

## 9. 其余用例执行要点

- **发送**：SND-002/003/004 放高限流；SND-005 用 `GROUP_OUTBOX_WORKERS=0` 证明“提交即返回”，
  同时查 staging 正文与 outbox 引用；SND-006/007/020 按字节构造（4 字节 emoji 填 65536）；
  SND-015 临时触发器注入 outbox 失败，断言 staging 一并回滚；SND-022 注入 staging 失败，或在 Mongo
  无 doc 时重跑 worker，断言正文来自 staging；SND-016 持成员行锁改 `active=0` 再提交，发送应读最新值而非快照。
- **幂等**：IDM-001/006 同 clientID 串行/断线重发；IDM-007 用 `updated_at=now-6d/8d` 构造 outbox 窗口，
  staging 用 `created_at=now-25h` 验证 24h 清理；IDM-008 对 Create/Apply/Review/Kick 各重放一次并比对
  `Reply` 与 DB；IDM-009 在 worker 暂停时重发，断言正文从 staging 取。
- **扇出**：FAN-005/006/016 用 fixture 精确造 200/201 active，内联同时看“收件人数”和“单条≤256KiB”；
  FAN-007/008 用 `DEL/HSET grp:members`；FAN-010/011 重投事件，客户端按 `serverMsgID` 去重；
  FAN-014/015 停 Push/塞满写队列，恢复后 245 补齐。
- **拉取**：PULL-003 snapshot 上界取请求时 `published_seq`；PULL-005 limit 0/101 归一化；PULL-006
  after 越界返回空；PULL-013 边拉边发，每页按 snapshot 截断；PULL-019 用 tombstone fixture；
  PULL-020 丢弃 251 后验证 30s 主动 245（测试可手动触发一次对账）。
- **同步/成员**：SYNC-003 待补 1..20 → 一帧 251，>20 转 needsPull；SYNC-006 51 群取 50 且 `hasMore`；
  SYNC-009 让单群查询失败看 `PARTIAL`；SYNC-012 边同步边发；SYNC-014 把 `next_seq` 调大、`published_seq`
  保持较小构造“已分配未发布”，断言 needsPull；MEM-003/009/010 每次变更核对 `epoch/join_seq/active/count`；
  MEM-014 200 active+1 inactive 分页；MEM-015/016 第 2000 人成功、第 2001 人 `GROUP_FULL`，群号回填跑两遍；
  MEM-018 并发审批/退踢后 count=active 数。
- **故障/性能**：FLT-001/009/010 先断言 246 再断言 outbox/Mongo/stream；FLT-004/005/008 `kill -9` 后用
  `serverMsgID` 唯一索引断言“只 1 doc”；FLT-010 先让 Mongo 不可用并超过 attempts（断言仍 `stage=0`、不 state=3），恢复后断言 tombstone 文档与 `published_seq`；FLT-011 断言无 tombstone；
  FLT-013 构造 8 天前 state=2/3 与 state=0/1，以及 25h 前 staging 孤儿；FLT-014 `kill -STOP` 依赖 5s；
  PERF 每场景 3 次取中位数，记录 p50/p99、TPS、锁等待、lag、资源。
- **回归/安全**：REG-001 每轮先跑单聊脚本，失败即停；REG-007 直接查 `information_schema` 断言新表/新列；
  SEC-004 发 >1MiB 帧后验证进程与重连。

---

## 10. 自动化方案

### 10.1 新脚本 `scripts/smoke/group_delivery_test.py`

复用 `group_test.register`、`group_test.login` 的注册/登录逻辑，`ws_test.WS` 的帧收发，`ack_test.py` 的
`sql()/wait_for()/--json` 结构。默认串行，避免 fixture 相互干扰。

```python
CASES = [
    ("SND-001", "小群单条发送", case_snd_001),
    ("SND-005", "提交即返回/staging", case_snd_005),
    ("SND-022", "staging 与 outbox 同事务", case_snd_022),
    # ... 137 条核心用例按 §7 编号注册
]
```

命令行：`--case SND-001 --case PULL-004`、`--faults`（破坏性）、`--perf`、`--json /tmp/group-results.json`；
失败打印 `gid/seq/serverMsgID` + SQL/Mongo/帧快照，退出码 0=无 FAIL。

### 10.2 新 helper（246/245/210）

```python
def wait_frame(ws, types, timeout=5, stash=None):
    while True:
        f = ws.recv_proto(timeout=timeout)
        if f is None: raise AssertionError(f"timeout waiting {types}")
        if f.type in types: return f
        (stash if stash is not None else []).append(f)
def rpc(ws, ftype, body, resp_cls, timeout=5):
    req = sdkws_pb2.SdkWSReq(type=ftype, requestId=f"rpc-{time.time_ns()}")
    req.data = body.SerializeToString(); ws.send(req.SerializeToString())
    frame = wait_frame(ws, {ftype}, timeout)
    resp = resp_cls(); resp.ParseFromString(frame.data)
    return frame, resp
def send_group(ws, gid, content, client_id, content_type=101):
    return rpc(ws, 246, groupchat_pb2.SendGroupMessageReq(
        groupID=gid, clientID=client_id, content=content, contentType=content_type),
        groupchat_pb2.SendGroupMessageResp)[1]
def pull_group(ws, gid, after_seq, limit=20, snapshot=0, before_seq=0):
    return rpc(ws, 245, groupchat_pb2.GetGroupChatHistoryReq(
        groupID=gid, afterSeq=after_seq, limit=limit, snapshot=snapshot, beforeSeq=before_seq),
        groupchat_pb2.GetGroupChatHistoryResp)
def wait_sync(ws, timeout=8):
    """登录后收集 251 直到 210；返回 (notices, summary, order)。"""
    notices, order = [], []
    while True:
        f = ws.recv_proto(timeout=timeout)
        if f is None: raise AssertionError("missing 210")
        order.append(f.type)
        if f.type == 251:
            n = groupchat_pb2.Notice(); n.ParseFromString(f.data); notices.append(n)
        elif f.type == 210:
            g = groupchat_pb2.GroupSyncSummary(); g.ParseFromString(f.data)
            return notices, g, order
```

### 10.3 登录 helper 必须等 210

现有 `group_test.login` 只消费到 `209` 就返回，会把随后的 251/210 留在 socket 里；
v3 脚本应实现 `login_v3`：发送 91 → 收 209（单聊）→ 继续收 251/210 直到 210，
并把 `209/210` 的 `errorCode` 与 `order` 一起返回。**不要**把 210 当 209，也不要把 251 当 209。

### 10.4 fixture/观测 helper
```python
def fixture_group(gid, owner, members, next_seq=0, published_seq=None, epoch=1, dismissed=0): ...
def seed_history(gid, seqs, sender, committed=True, is_deleted=False): ...  # 只写 Mongo 文档
def seed_tombstone(gid, seq): ...                          # isDeleted=true, committed=true
def stream_row(gid): ...                                   # next_seq/published_seq/...
def staging_rows(gid=None, msg_id=None): ...
def outbox_rows(gid): ...                                  # 含 payload 引用断言
def mongo_count(gid, committed=None, deleted=None): ...
def cleanup_group(gid): ...                                # §3.4 顺序
def login_v3(port, account): ...
```
断言统一走“等条件”而不是 sleep：`wait_for(lambda: mongo_count(gid) == 1, 10)`、
`wait_for(lambda: stream_row(gid)["published_seq"] == seq, 10)`。

### 10.5 故障注入

```bash
# 停 Kafka / Redis / Mongo（容器名按实际调整）
docker stop kafka-standalone
docker stop redis-standalone
docker pause mongo-standalone          # 模拟 Mongo 挂起；恢复用 unpause
# 停/挂起服务
pkill -9 -f 'build/main --role=group'
pkill -9 -f 'build/main --role=push'
pkill -9 -f 'build/main --role=gateway.*--ws-port=6667'
kill -STOP $(pgrep -f 'build/main --role=group'); kill -CONT ...
# 网关 push 不可达（Unknown）
iptables -A OUTPUT -p tcp --dport 50075 -j DROP
# 恢复
bash scripts/smoke/start_group.sh
```

每个故障用例 `finally` 恢复；断言“最终可 245”，不要只断言“推送成功”；
记录故障窗口的 outbox `stage/state/attempts`、staging 行数、Mongo `committed`/`isDeleted`、
`published_seq`、Kafka lag。

### 10.6 与现有测试的关系

| 脚本/测试 | 关系 |
|---|---|
| `ack_test.py` / `seq_test.py` | 单聊回归，必须继续通过（REG-001）；群没有 ACK 可测 |
| `group_test.py` | 现有 250 建群/审批链路可复用；`Send` 需改 246，`login` 需等 210 |
| `test/group_integration.cpp` | 现有 v2 断言（250 Send/Pull/Ack/Read/unread）需重写为 246/245 + Mongo；本期只保成员/群号/幂等部分，ACK/read 断言删除 |
| `all_test.py` / `drain_test.py` | 单进程/下线回归，不得因群帧变更失败 |
| 新增 `group_delivery_test.py` | 本文 137 条核心用例的载体 |


---

## 11. 执行顺序

1. **冒烟**：SND-001、FAN-001、PULL-001、SYNC-001/002、MEM-001/003、REG-001。
2. **P0 基础**：SND-002/003/005/008/009/010/016/017/018、SND-022、IDM-001/002/005、FAN-003/004/012、
   PULL-002/004/007/012/019、SYNC-004/005/014、MEM-007/009/010/011、SEC-001/002/003。
3. **P0 故障**：FLT-001/002/003/004/006/009、PERF-001。
4. **P1 边界**：其余 SND/IDM/FAN/PULL/SYNC/MEM/FLT（含 PULL-020、FLT-010/011/013）。
5. **P2 规模**：PERF-002..008、IDM-007、PULL-005 等。
6. 每轮结束跑单聊 `ack_test.py`/`seq_test.py`；群用例失败先查“246 是否成功、Mongo 是否 committed、
   `published_seq` 是否推进、210 是否发出”。

---

## 12. 退出标准

- 核心 P0 全部 PASS；P1 PASS 或经评审记录为已知限制（有告警/监控/文档）；
- 单聊回归 `ack_test.py`、`seq_test.py` PASS；`group_test.py` 适配 246/210 后 PASS；
- v3 群集成测试（`group_integration.cpp` 重写版）覆盖成员/群号/seq/幂等并通过；
- **水位**：所有 `lastSeq` 断言等于 `published_seq`；`next_seq` 只作分配水位；
  `needsPull=(toSeq<lastSeq)` 场景 PASS；客户端不拿 `lastSeq` 推进 cursor；
- **staging**：发送事务内 staging 与 outbox 同生共死；worker 写 Mongo 后删 staging；
  outbox `payload` 无正文；staging 孤儿按 24h 清理；
- **tombstone**：outbox 放弃时写 `isDeleted=true/committed=true` 并推进 `published_seq`；
  245 返回 `deletedSeq`；客户端可跳过且不卡 cursor；本期不支持撤回/删除；
- **前台对账**：30s `245(afterSeq=cursor,limit=20)` 能补 251 丢失；cursor 只按连续消息推进；
- 无 P0/P1 缺陷：消息丢失（非 tombstone 可表达的永久空洞）、seq 回退/分配空洞、`next_seq` 当可见水位、
  staging/outbox 半写、越权、重复入群、解散后新消息；
- 故障演练：MySQL/Mongo/Kafka/Redis/Push/Group/网关各停一次，消息最终可 245，无脏数据；
  FLT-010 后 tombstone 可拉、无不可见空洞；
- 24h 长稳：outbox/request/staging 有界、lag 可回落、无内存泄漏；
- **群没有 ACK/107、没有服务端未读**；退出条件覆盖核心 137 条。

---

## 13. 风险与待确认

| # | 点 | 说明/处理 |
|---|---|---|
| 1 | 200/201 内联阈值口径 | **已统一**：按 active 成员数（排除 sender 前）判断；≤200 内联，201→hint。FAN-005/006 按此断言 |
| 2 | 同步内联字节 | **已统一**：每群内联按 `GROUP_INLINE_MAX_BYTES=256KiB` 截断条数；0 条能放下则 `needsPull`；总消息≤200 |
| 3 | `GroupSyncRange.fromSeq/toSeq/hasGap` | **已统一**：`floor=max(cursor,joinSeq)`、`fromSeq=floor+1`、`toSeq=内联实际覆盖到的 seq（无内联=fromSeq-1）`、`needsPull=(toSeq<lastSeq)`、`hasGap=(fromSeq>cursor+1)` 即 `cursor<joinSeq` |
| 4 | `245.nextSeq` 空页 | **已统一**：空页回显 `from`；有消息时为最后一条 seq |
| 5 | `state=3` 可 Pull 条件 | **已修正**：FLT-010 放弃写 → tombstone（`isDeleted` 可拉）；FLT-011 已发布 → `committed=true` 可拉；两者都无不可见洞 |
| 6 | `group_test.login` | 只等到 209，不适用于 v3 登录；新脚本用 `login_v3`，不修改现有脚本 |
| 7 | `group_integration.cpp` | 仍是 v2（Send/Pull/Ack/Read/unread、群消息在 MySQL）；需实现阶段重写，本期文档不要求其现状通过 |
| 8 | 单机资源 | 2000 人全在线不可压；用 fixture+少量在线验证逻辑，性能结论标注“单机” |
| 9 | Kafka 分区 | 测试 topic 至少 8 分区，Unknown/热群用例才可复现；生产 32；单群仍串行（ADR-17） |
| 10 | tombstone/`deletedSeq` | **已支持**：outbox 放弃写 tombstone，245 返回 `isDeleted` + `deletedSeq`，客户端跳过正文但推进 cursor；**本期不支持撤回/删除**，永久空洞只来自 outbox 放弃 |
| 11 | `published_seq` 不连续 | `published_seq` 是“最大已发布”，不是连续可见水位（允许中间 gap）；客户端只按实际拉到的连续消息推进 cursor |
| 12 | staging 孤儿/丢失 | staging 与 outbox 同事务；stage=1 后删除；孤儿按 24h 清理；丢失时回读 Mongo（stage=0 重试） |
| 13 | 客户端契约 | 服务端保证不丢可拉；客户端必须去重、按 seq 排序、buffer 后重拉；前台每 30s 245 对账 |
| 14 | 解散/踢人通知 | 251 是 best-effort；离线成员靠登录后 List/210 状态判断，不保证实时收到 |

---

## 14. 真实场景串联（E2E）

> 6 个场景全部按 v3：246 发送、Mongo 存储、251/210/245，全程无 ACK/无服务端未读。

### E2E-001 三人群日常（跨网关）
A（owner，gw-1）、B（gw-2）、C（gw-2）。
1. A 建群，B/C 申请、A 审批（250）；
2. A 发 3 条（246，seq 1–3），B/C 各收 251 内联；A 不收；
3. C 断线；B 发 2 条（4–5）；A 收 251；
4. C 重连 `91`：209（单聊）→ 251 内联 4–5 → 210（`lastSeq=5, needsPull=false`）；
5. A 再发 2 条（6–7）；核对 `next_seq=7`、`published_seq=7`、Mongo 7 doc、seq 1..7 连续。
**期望**：C 补齐 4–5；未读由客户端 `lastReadSeq` 算（登录前 2，渲染后 0），服务端无 unread。

### E2E-002 新成员入群边界
1. 群已有 1..10；B 申请（`joinSeq=10`）；
2. A 发 11；B 245 只看到 11；
3. B 退群；A 发 12；B 重入（`joinSeq=12`）；
4. B 245：1..12 不可见；A 发 13，B 看到 13；`epoch` 每次变更 +1。
**期望**：joinSeq/epoch 正确，旧消息不可见，重入从 13 开始。

### E2E-003 大群 500 人（1 人在线）
1. fixture 500 active，仅 B 在线；A 发 1 条；
2. B 收 251 hint（无正文）；B 245 拉到；
3. 另 10 人登录：210 `needsPull=true`，各自 245 分页补齐；
4. 检查 Mongo 只 1 doc/消息、Kafka lag 回落、outbox 清空、`published_seq` 到位。
**期望**：在线 1s 内 hint；离线登录补齐；存储/写不随成员数放大。

### E2E-004 群解散
1. 群正常，A 发若干消息；B 在线、C 离线；
2. A Dismiss；B 收 251 `reason=2, refresh=true`；C 登录后 List 看到 dismissed；
3. B/C 发 246/245 → `GROUP_DISMISSED`；无 ACK；
4. 群从 active 列表消失；pending 申请关闭。
**期望**：解散后无新消息/新成员；在线通知尽力而为；离线靠状态查询。

### E2E-005 Kafka 故障 30s
1. 停 Kafka；A 连发 5 条；B 在线但无 251；
2. B 主动 245 能拉到 5 条（Mongo `committed=true`，`published_seq` 已推进）；
3. 恢复 Kafka；outbox 重试到 state=2，B 收补推（或已拉取去重）；无 tombstone；
4. C 登录：210 needsPull，245 补齐。
**期望**：发送不失败、消息不丢、恢复后实时性恢复；无重复消息（客户端去重）。

### E2E-006 群主踢人 + 重入
1. A 踢 B；B 收 251 `reason=3`（尽力）；缓存 DEL；
2. B 发 246/拉 245 → `NOT_MEMBER`；
3. B 重新申请、A 审批；`epoch+1`、`joinSeq=当前 next_seq`；
4. B 看不到被踢前消息；B 发消息正常，其他成员收 251。
**期望**：权限/可见性/缓存失效正确；无 ACK 参与。

---

> 群聊**没有 ACK/107**；未读由客户端本地 `lastReadSeq` 维护；永久空洞由 outbox tombstone 表达，
> 客户端以 `isDeleted` 跳过正文、以实际拉到的连续消息推进 cursor。