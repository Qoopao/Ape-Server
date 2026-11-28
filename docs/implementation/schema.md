# 数据模型与存储

> 状态：现状  
> 权威性：本文是「MySQL 表、Mongo 集合、Redis key、Kafka topic、etcd key」的唯一索引来源  
> 最后核对：2026-11-26 @ `03ed7c2` + 工作区多网关/群聊改造  
> 适用代码：`src/storage/`、`scripts/sql/`、`proto/`

> 本文是**索引**，不是完整 DDL。建表/补列以 `MySQLHandler::Initialize*` 和 `scripts/sql/*.sql` 为准；群聊详细字段见 [design/group-chat.md](../design/group-chat.md) 附录 B。

## 1. MySQL（库 `ape_auth`）

### 1.1 账号与注册

| 表 | 关键字段/索引 | 用途 |
|---|---|---|
| `users` | `user_id` PK、`account` UK、`phone` UK、`nickname`、`password_hash`、`password_salt`、`last_login_at` | 账号主体 |
| `account_pool` | `account` UK、`status`、`idx_status_id(status,id)` | 预生成 10 位账号池 |
| `account_pool_cursor` | `id=1`、`next_start` | 号段游标 |
| `account_pool_refill_lock` | `lock_name` PK、`holder`、`expires_at` | 补货分布式锁 |
| `registration_requests` | `request_id` PK、`UNIQUE(user_id)`、`UNIQUE(account)` | 注册幂等 |

### 1.2 好友

| 表 | 关键字段/索引 | 用途 |
|---|---|---|
| `friends` | `UNIQUE(owner_account,friend_account)`、`owner_list(owner_account,id)` | 双向两行表示好友 |
| `friend_pairs` | PK(low_account,high_account) | 好友对行锁，写互斥 |
| `friend_applications` | `incoming(to,id)`、`outgoing(from,id)`、`pending(from,to,status)` | 好友申请 |
| `friend_chat_deletions` | PK(owner_account,peer_account)、`deleted_seq` | 删除水位；历史不可见下界 |

### 1.3 会话与单聊可靠性

| 表 | 关键字段/索引 | 用途 |
|---|---|---|
| `im_conversations` | PK(owner_account,conversation_id)、`conversation_order(owner,is_hidden,is_pinned,latest_time,conversation)` | 会话列表/未读/摘要 |
| `im_conversation_unread` | PK(owner,conv,seq)、FK→`im_conversations` | 未读明细 |
| `im_conversation_seq` | `conversation_id` PK、`next_seq` | 单聊每会话稠密 seq |
| `im_conversation_receipts` | PK(owner_account,conversation_id)、`delivered_seq` | 单聊 ACK 水位 |
| `im_outbox` | `UNIQUE(operation,dedupe_key)`、`outbox_ready(state,available_at,id)`、`outbox_lease(state,lease_until,id)`、`outbox_message(server_msg_id,operation)` | 可靠投递状态机 |

`im_outbox.operation`：`1` 单聊发送、`2` 好友删除后历史清理、`3` ACK 后状态清理、`4` 群聊发送。
`state`：0 待执行、1 已领取、2 完成、3 取消。`stage`：0 正文未提交、1 正文已提交。
`im_outbox.traceparent`：可观测列（`VARCHAR(96) NOT NULL DEFAULT ''`），保存受理 span 的 W3C traceparent，供异步投递段建 link；不参与业务判定，代码回滚时保留列即可。

### 1.4 群聊（v3）

| 表 | 关键字段/索引 | 用途 |
|---|---|---|
| `im_groups` | PK(id)、`owner`、`member_count`、`dismissed` | 群元数据；**不再有 `last_seq`** |
| `im_group_stream` | PK(group_id)、`next_seq`、`published_seq`、`epoch`、`dismissed` | 每群稠密 seq/发布水位/准入 |
| `im_group_members` | PK(group_id,account)、`account_active(account,active,group_id)`、`join_seq`、`epoch` | 成员与入群水位；**无 ack/read** |
| `im_group_applications` | `group_page(group_id,id)`、`applicant(account,id)` | 入群申请 |
| `im_group_requests` | PK(actor,client_id)、`payload`、`reply` | 群命令幂等回放 |
| `im_group_numbers` | `group_id` PK、`number` UK | 10 位群号 |
| `im_group_number_cursor` | `id=1`、`next_number` | 群号游标 |
| `im_group_send_staging` | `server_msg_id` PK、`group_id`、`seq`、`payload` | 正文暂存；与 outbox 同事务，worker 写 Mongo 后删除 |

`im_group_messages` 是**旧表**，v3 不再读写；老库由 `InitializeGroups` 检测后重建，或执行 [scripts/sql/drop_legacy_groups.sql](../../scripts/sql/drop_legacy_groups.sql)。完整 DDL 见 [scripts/sql/group_chat.sql](../../scripts/sql/group_chat.sql)。

### 1.5 迁移与重置

| 脚本 | 用途 |
|---|---|
| `scripts/sql/friends.sql` | 好友相关表 |
| `scripts/sql/conversations.sql` | 会话相关表 |
| `scripts/sql/outbox.sql` | outbox 表/索引 |
| `scripts/sql/group_chat.sql` | 群聊 v3 schema |
| `scripts/sql/drop_legacy_groups.sql` | 破坏性重置：删旧群表（停服务后执行） |

规则：迁移必须幂等、可重入、可回滚；先在空库/快照演练；不要在共享环境直接 `DROP`。

## 2. MongoDB（`IM-System.msg`）

单聊与群聊正文共用集合：

```js
{
  serverMsgID, sendID, recvID, convID, clientMsgID,
  senderPlatformID, senderNickname, senderFaceURL,
  sessionType, msgFrom, contentType, content,
  seq, sendTime, status, isRead, isDeleted, isRecalled, isPinned,
  isGroupMsg, ext, committed
}
```

| 项 | 约定 |
|---|---|
| 单聊 | `recvID=B`、`convID=min_max`、`isGroupMsg=false` |
| 群聊 | `recvID=0`、`convID=groupID`、`isGroupMsg=true`、`sessionType=2` |
| 写入 | `update_one({serverMsgID}, {$setOnInsert: doc}, {upsert:true})` 幂等 |
| 索引 | `{convID:1,seq:1}`、`{recvID:1,seq:1}`、`UNIQUE {serverMsgID:1}` |
| 启动校验 | 同键但非 full-unique 的索引 → 拒绝启动，要求显式迁移 |
| 可见性 | `committed != false`；群聊以 `published_seq` 为同步上界 |

群聊额外约定：`isDeleted=true` 的 tombstone 表示永久空洞；客户端跳过正文但推进 cursor。详见 [design/group-chat.md](../design/group-chat.md)。

## 3. Redis

| key | 值 | TTL | 用途 |
|---|---|---|---|
| `token:{token}` | `"{userID},{account}"` | 7 天 | 登录令牌 |
| `gw:user:{account}` | `"{instanceId}:{sessionId}"` | 300s | 网关路由；心跳 CAS 续期；断线不删，靠 TTL |
| `msg:{serverMsgID}` | `MsgData` protobuf | 7 天 | 单聊在线投递缓存；miss 回源 Mongo |
| `grp:members:{gid}` | HASH account→joinSeq | 600s | 群成员缓存（若启用；以代码为准） |

**不要**把 Redis 当在线/送达的正确性判据；旧 `user:{account}:online`、`pending_ack:*`、`msg_conv:*`、`msg:consumed:*` 属于遗留/待清理。

## 4. Kafka

| topic | key | 消费组 | 用途 |
|---|---|---|---|
| `msg_topic` | `conversationID` | `push-service-group` | 单聊在线投递 |
| `group_topic` | `groupID` | `push-group-fanout` | 群聊扇出 |

- 单聊消息体：`serverMsgID` 纯文本；消费端兼容 `traceparent|serverMsgID`。
- 群聊消息体：`traceparent|serverMsgID`；正文在 Mongo，不进 Kafka。
- 顺序：同一 key 固定分区，lane 内串行；失败不提交 offset，由下游幂等吸收重复。

## 5. etcd

| 项 | 约定 |
|---|---|
| 端点 | `http://127.0.0.1:2379`（开发） |
| key | `/services/{ServiceName}/{instanceId}` |
| value | JSON：`{"instance_id","address","methods"}` |
| 租约 | 300s + KeepAlive；watchdog 30s 检查并重新注册 |

注册地址必须是可路由地址，不能是 `0.0.0.0`。

## 6. 一致性与权威

| 数据 | 权威 | 说明 |
|---|---|---|
| 单聊正文 | MongoDB | `$setOnInsert` 幂等；`committed` 标记发布 |
| 单聊投递状态 | MySQL `im_outbox` | 事务 + 行锁 + 租约 |
| 单聊 ACK | MySQL `im_conversation_receipts` | 水位只增不减 |
| 群聊正文 | MongoDB 单副本 | 一条群消息一份文档 |
| 群聊 seq/发布水位 | MySQL `im_group_stream` | `next_seq` 已分配；`published_seq` 已发布 |
| 群聊投递 | Kafka + PushService | 尽力而为；Pull 兜底；无 ACK |
| 在线/路由 | 网关返回值 + Redis 路由 | 路由只影响性能 |
| 服务注册 | etcd | 租约 + KeepAlive |

## 7. 相关文档

- 群聊详细 schema/协议：[design/group-chat.md](../design/group-chat.md)
- 单聊可靠性：[design/messaging.md](../design/messaging.md)
- 协议：[protocol.md](../protocol.md)
- 功能状态：[status.md](status.md)
