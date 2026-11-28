# 账号、好友与会话设计

> 状态：现状  
> 权威性：本文是「账号/好友/会话域设计规则」的唯一来源  
> 最后核对：2026-11-26 @ `03ed7c2` + 工作区改造  
> 适用代码：`src/services/auth_service/`、`friend_service/`、`conversation_service/`、`src/storage/mysqlhandler.cpp`

## 1. 账号与认证

- 注册：11 位手机号 + 昵称 + 密码；服务端校验 `requestId`、手机号、长度；账号从 `account_pool` 取 10 位号。
- 幂等：`registration_requests.request_id` 唯一；同请求 ID 重试返回同一账号。
- 登录：`type=91`，`token` 为空时 `data` 为明文密码字节；成功签发 UUID token，写 Redis `token:{token}`（7 天）。
- 令牌：`ValidateToken` 返回 `{valid,userID,account,expiresAt}`，且 `account` 必须与请求一致。
- 密码：当前 `SHA-256(salt||password)` 单轮；**已知缺口**，目标 Argon2id/bcrypt/PBKDF2 + 恒定时间比较。
- 互斥登录：同一账号同一时刻一个活跃连接；新登录踢旧连接（同实例 + 跨实例）。

### 不变量

1. 账号/手机号唯一；注册幂等。
2. `sendID`/`actor` 永远由服务端按连接账号覆写。
3. token 失效/Redis 不可用时，登录返回明确错误，不得静默放行。
4. 密码/令牌不得出现在日志。

## 2. 好友

单入口 `FriendService.Execute(Command)`，`operation` 6 个：

| operation | WS type | 语义 | 关键规则 |
|---|---|---|---|
| `LookupUser` 0 | 220 | 查公开信息 | `target != 0`；返回 `userID/nickname` |
| `Apply` 1 | 221 | 发起申请 | 不能加自己；目标存在；已好友 → `ALREADY_FRIEND`；同向 pending 复用；`message ≤512` |
| `ListFriends` 2 | 222 | 好友列表 | `limit clamp(1,100)`；首页固定 `snapshot=MAX(id)`；`id > after AND id <= snapshot` |
| `ListApplications` 3 | 223 | 申请列表 | 收发双向；同上分页；不过滤状态 |
| `HandleApplication` 4 | 224 | 同意/拒绝 | 仅 `recipient` 可处理；同终态幂等；冲突 → `ALREADY_HANDLED` |
| `DeleteFriend` 5 | 225 | 删除好友 | 双向删两行；写双方 `friend_chat_deletions`；pending 置 Rejected；清会话；排 outbox(op=2) 清理历史 |

### 不变量

1. 好友关系必须存在 `(A,B)` 与 `(B,A)` **两行**，`COUNT(*)==2` 才算好友。
2. 所有写操作先 `lockPair(low,high)`，对 `friend_pairs` 主键行取排他锁；隔离级别 READ COMMITTED。
3. 消息可见性：好友双向成立 + `seq > friend_chat_deletions.deleted_seq`。
4. 删除后关系恢复，旧消息**仍不可见**（设计如此）。
5. 变更通知：成功后向 `changedAccounts` 在线用户推 `230`；离线靠登录全量同步。

## 3. 会话

| RPC | WS type | 语义 | 规则 |
|---|---|---|---|
| `GetAllConversations` | 240 | 全部未隐藏会话 | `is_pinned DESC, latest_time DESC, conversation_id DESC` |
| `MarkConversationRead` | 241 | 标记已读 | `readSeq > 0 && <= max_seq`；重复/低水位幂等；删未读明细并回减计数 |
| `UpdateConversationSettings` | 242 | 置顶/免打扰 | `isPinned`、`recvMsgOpt` optional；`recvMsgOpt ∈ {0,1}` |
| `DeleteConversation` | 243 | 隐藏会话 | 软删：`is_hidden=1, read_seq=max_seq, unread_count=0`；新消息复位 |

> 2026-11-27：`conversation.proto` 只保留网关 240–243 使用的这 4 个 RPC；`GetConversation`、`GetSortedConversationList` 及未实现 RPC 已移除（未参与网关链路）。`Conversation`/`MsgInfo` 中未落库的群聊/扩展字段已删除，剩余字段号重排为连续编号（1..N）。这是 wire-breaking 变更：旧客户端必须同步重新生成 `conversation.proto`，否则 240 响应会解析错位。
>
> 240–243 的响应都带 `conversation.OperationResult result`：`errorCode="0"` 成功；预期内业务失败（`INVALID_PARAM`/`INVALID_POSITION`/`NOT_FOUND`）通过 `result` 返回，gRPC 仍为 OK；DB/传输失败仍走 gRPC status，网关映射为 `CONVERSATION_ERROR`。

### 不变量

1. 会话行唯一创建路径：单聊消息落库时 `RecordConversationMessage` 对收发双方各 upsert 一行。
2. `max_seq` 用 `GREATEST` 单调推进；`read_seq <= max_seq`。
3. 未读明细只对接收者插入；`unread_count` 单调 +1 / 按删除行数回减。
4. 隐藏会话收到新消息时 `is_hidden` 复位 0。
5. `min_seq`/清空会话语义当前不完整，属于已知缺口。

## 4. 已知缺口

- 好友黑名单、备注写入、好友上限、申请频控。
- 显式创建会话、清空会话、群会话统一模型。
- 密码 KDF、token 吊销/续期、登录限流。
- `INVALID_PARAM` 一词多义，客户端难以区分「重新登录」还是「改参数」。

## 5. 相关文档

- 协议：[../protocol.md](../protocol.md)
- 数据模型：[../implementation/schema.md](../implementation/schema.md)
- 功能状态：[../implementation/status.md](../implementation/status.md)
- 网关/多实例：[gateway.md](gateway.md)
