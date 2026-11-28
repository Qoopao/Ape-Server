# 通信协议

> 状态：现状  
> 权威性：本文是「WebSocket type 码、帧语义、错误码、幂等、超时、兼容规则」的唯一来源  
> 最后核对：2026-11-26 @ `03ed7c2` + 工作区多网关/群聊改造  
> 适用代码：`proto/*.proto`、`src/gateway/ws_session.cpp`、`src/services/`

## 1. 传输层

| 项 | 约定 |
|---|---|
| 协议 | WSS（TLS 1.2+ over TCP），**二进制帧** |
| 端口 | 网关默认 `6666` |
| 握手 | TLS handshake → WebSocket upgrade；两步都成功才可读业务帧 |
| 帧上限 | 依赖 Beast 默认；建议显式配置 `read_message_max`（当前未强制） |
| 路径/Origin | 当前不校验 URL path、Origin、子协议；属于已知缺口 |
| 序列化 | 所有 WS 帧为 protobuf `sdkws.SdkWSReq` / `sdkws.SdkWSResp` 二进制 |

客户端不应发送文本帧；服务端不解析非 protobuf 数据（认证后仅记 debug 并继续）。

## 2. 外层信封

请求 `sdkws.SdkWSReq` 关键字段：

| 字段 | 号 | 用途 |
|---|---|---|
| `requestId` | 1 | 请求-响应关联；原样回显 |
| `token` | 2 | 仅登录帧使用；非空走 token 校验 |
| `account` | 3 | 登录账号；其余帧以连接绑定账号为准 |
| `deviceID` | 4 | 当前仅回显，不参与会话键 |
| `data` | 5 | 业务载荷，类型由 `type` 决定 |
| `type` | 6 | 请求类型码 |
| `trackID` | 7 | 日志用 |
| `cursors` | 8 | 每会话水位（`conversationID + lastSeq`），登录帧携带 |

响应 `sdkws.SdkWSResp` 关键字段：`requestId`、`token`、`account`、`deviceID`、`errorCode`、`errorMsg`、`data`、`type`、`logID`。`logID` 回填服务端 trace id（32 位 hex）：网关对 101/244/245/246 响应与 104 推送帧赋值；客户端报障时提供 `logID`，服务端可直接在 Jaeger 打开完整链路。其他类型或无 trace 时为空串。

**响应 type 规则**：请求-响应帧的 `type` 与请求一致；错误帧用 `type=0`；服务端主动下推的 type 见 §4。

## 3. 请求 type 码表（当前）

| type | 方向 | 语义 | 请求 data | 响应 |
|---|---|---|---|---|
| `0` | C→S / S→C | 心跳 ping / pong | 无 | `errorCode="0"`、`errorMsg="pong"` |
| `90` | C→S | 注册账号 | `auth.RegisterRequest` | `type=90`，回填 `account`；不签发 token |
| `91` | C→S | 登录 | `token` 为空时 `data` 为明文密码字节；否则忽略 data | `type=91`，回填 `token`、`deviceID` |
| `101` | C→S | 单聊发送（可批量） | `sdkws.SendMessageReq` | `type=101`，`sdkws.SendMessageResp` |
| `106` | C→S | 按会话水位 ACK | `sdkws.AckReq{ackType, conversations[]}` | **无响应帧** |
| `220–225` | C→S / S→C | 好友 6 个操作 | `friendpb.Command` | `type` 同请求，`friendpb.Reply`；`operation = type - 220` |
| `240` | C→S / S→C | 拉取全部会话 | 无 | `conversation.GetAllConversationsResp`（含 `result`） |
| `241` | C→S / S→C | 标记已读 | `conversation.MarkConversationReadReq` | `conversation.MarkConversationReadResp`（含 `result`） |
| `242` | C→S / S→C | 更新会话设置 | `conversation.UpdateConversationSettingsReq` | `conversation.UpdateConversationSettingsResp`（含 `result`） |
| `243` | C→S / S→C | 隐藏会话 | `conversation.DeleteConversationReq` | `conversation.DeleteConversationResp`（含 `result`） |
| `244` | C→S / S→C | 单聊历史 | `msg.GetSingleChatHistoryReq` | `msg.GetSingleChatHistoryResp` |
| `245` | C→S / S→C | 群历史（v3） | `groupchat.GetGroupChatHistoryReq` | `groupchat.GetGroupChatHistoryResp` |
| `246` | C→S / S→C | 群发送（v3） | `groupchat.SendGroupMessageReq` | `groupchat.SendGroupMessageResp` |
| `250` | C→S / S→C | 群命令（成员/元数据） | `groupchat.Command` | `groupchat.Reply` |
| `251` | S→C | 群变更/新消息通知 | — | `groupchat.Notice` |

**已废弃/不支持**：

- `250` 的 `Send(11)` / `Pull(12)` / `Ack(13)` / `Read(14)`：返回 `UNSUPPORTED_OPERATION`，改用 `246/245/210`。
- `102`/`103`/`105`：协议注释里的历史占位，当前无实现。
- `lastMsgSeq`：字段已从 `sdkws.SdkWSReq` 删除；使用 `cursors`（字段 8）。

**重要**：登录固定 `91`、注册固定 `90`；不得恢复 `101` 的登录语义。`101` 认证后只表示单聊发送。

## 4. 服务端主动下推

| type | 语义 | data | 说明 |
|---|---|---|---|
| `104` | 在线单聊消息 | `sdkws.MsgData` | `PushFrame`/`PushBatch` |
| `200` | 离线单聊补投（单条） | `sdkws.MsgData` | `requestId=serverMsgID` |
| `209` | 单聊登录同步完成 | `sdkws.SyncSummary` | `errorCode ∈ 0/PARTIAL/SYNC_ERROR` |
| `210` | 群聊登录同步完成 | `groupchat.GroupSyncSummary` | 在 `209` 之后；即使空也发 |
| `230` | 好友关系变更 | 无 | 客户端收到即全量刷新 |
| `251` | 群新消息/变更/刷新 | `groupchat.Notice` | 小群内联，大群 hint + Pull |

## 5. ACK 语义（单聊 `106`）

```proto
message ConversationAck {
  string conversationID = 1; // "min_max"
  int64 upToSeq = 2;         // >0，连续已处理水位
}
message AckReq {
  uint64 userID = 1;                  // 服务端以连接账号为准
  repeated ConversationAck conversations = 2;
  int32 ackType = 3;                  // 1=在线 2=离线；当前行为一致
}
```

规则：

- ACK 是**每会话连续水位**，不是逐条回执、不是已读。
- 无响应帧，fire-and-forget；客户端不等待、不用 ACK 结果推进 cursor。
- 服务端 `delivered_seq = GREATEST(delivered_seq, applied)`；只增不减。
- `upToSeq > max_seq` 静默截断；`upToSeq<=0` 过滤；会话不存在/非参与者 no-op。
- 单帧最多 100 个会话；网关按 100 个/批拆成多次 `AckConversations`。
- 群聊没有 ACK；不要对群会话发 `106`。

## 6. 错误码

错误码为字符串字面量，客户端已依赖的码不得随意改。常见码：

| 域 | 码 |
|---|---|
| 成功 | `""` / `"0"` |
| 协议/链路 | `401`、`4401`、`500`、`PARSE_ERROR`、`INVALID_PARAM`、`INVALID_MESSAGE` |
| 登录/注册 | `WRONG_PASSWORD`、`USER_NOT_FOUND`、`PHONE_EXISTS`、`ACCOUNT_RANGE_EXHAUSTED`、`ACCOUNT_POOL_UNAVAILABLE`、`REG_CLIENT_ERROR`、`REGISTER_FAILED` |
| 消息 | `PENDING`、`NOT_FRIEND`、`UPGRADE_REQUIRED`、`HISTORY_ERROR`、`DB_ERROR` |
| 同步 | `PARTIAL`、`SYNC_ERROR`、`SERVICE_UNAVAILABLE` |
| 多网关 | `KICKED`、`SERVER_DRAINING`、`SERVER_BUSY` |
| 好友 | `ALREADY_FRIEND`、`ALREADY_HANDLED`、`APPLICATION_NOT_FOUND`、`FORBIDDEN`、`INVALID_TARGET` |
| 会话（240–243 的 `result`） | `INVALID_PARAM`、`INVALID_POSITION`、`NOT_FOUND`；传输/DB 失败仍由网关映射为 `CONVERSATION_ERROR` |
| 群聊 | `GROUP_NOT_FOUND`、`GROUP_DISMISSED`、`NOT_MEMBER`、`OWNER_REQUIRED`、`OWNER_CANNOT_LEAVE`、`ALREADY_JOINED`、`INVALID_NAME`、`NAME_UNCHANGED`、`EMPTY_MESSAGE`、`INVALID_SEQ`、`MEMBERSHIP_CHANGED`、`IDEMPOTENCY_CONFLICT`、`GROUP_NUMBER_EXHAUSTED`、`INVALID_REQUEST_ID`、`APPLICATION_HANDLED`、`CACHE_ERROR`、`UNSUPPORTED_OPERATION` |

目标方向：统一定义 `enum ErrorCode`，停止新增数字码；`INVALID_PARAM` 拆为 `MISSING_ACCOUNT` / `TOKEN_INVALID` / `BAD_PAYLOAD`。

## 7. 幂等与关联

| 场景 | 关联键 | 行为 |
|---|---|---|
| 通用请求-响应 | `requestId` | 原样回显；不维护去重表 |
| 单聊发送 | `clientMsgID` | `im_outbox.dedupe_key = "{sendID}:{clientMsgID}"` 唯一；重复返回首次的 `serverMsgID/seq`；内容不同 → `INVALID_MESSAGE` |
| 注册 | `x-registration-id` gRPC metadata（由 `requestId` 透传） | `registration_requests.request_id` 幂等，重试返回同一账号 |
| 群命令 | `Command.clientID` | `im_group_requests(actor,client_id)` 存 hex(payload)+reply，重放上次结果 |
| 群消息 | `(group_id, sender, client_id)` | 唯一键；重复同内容返回原消息，内容不同 → `IDEMPOTENCY_CONFLICT` |
| 单聊 ACK | `(owner, conversationID)` 水位 | `GREATEST`，重复/低水位 no-op |

硬约束：`clientMsgID` ≤128、可打印 ASCII；单聊 `content` ≤64 KiB；群消息 `content` 非空 ≤64 KiB；超限返回 `INVALID_MESSAGE`，不得截断。

## 8. 超时与重试

| 项 | 值/行为 |
|---|---|
| `SendMessages` gRPC deadline | 10s |
| 好友/会话 RPC | 8s |
| 群 RPC | 10s |
| 历史拉取 | 10s |
| 单聊 ACK RPC | 8s |
| outbox 租约 | 45s；退避 `min(60s, 1s << attempts)` |
| Kafka 投递确认 | 12s（50ms 轮询） |
| 网关路由 TTL | 300s；心跳 CAS 续期 |
| `PushFrame` RPC | 单次 300ms；广播兜底整体约 300ms |
| 群 fanout | 尽力而为，不无限重试；Pull 兜底 |

## 9. 群聊 v3 帧

- `246 SendGroupMessageReq/Resp`：发送；`actor` 由网关覆写；返回 `MsgData{seq,serverMsgID}`。
- `245 GetGroupChatHistoryReq/Resp`：按 `afterSeq`/`beforeSeq`/`snapshot` 读 Mongo；返回 `messages/hasMore/nextSeq/minSeq/lastSeq/epoch/deletedSeq`。
- `210 GroupSyncSummary`：登录同步结束信号；`needsPull=(toSeq<lastSeq)`。
- `251 Notice`：小群（≤200）内联最近消息；大群只带 `lastSeq/epoch`，客户端 `245` 拉取。
- `250` 仅成员/元数据命令；旧 `11–14` 返回 `UNSUPPORTED_OPERATION`。

## 10. 演进约束

1. `proto/*.proto` 是唯一契约源；改 proto 后重新生成 `src/proto/`，禁止手改生成物。
2. 字段号只增不改；删除字段用 `reserved`；不得改字段类型/语义。
3. 新增 type 必须同时更新：网关分发、proto 载荷、本文、客户端最低版本说明。
4. 客户端契约变更必须服务端/客户端双方确认；不得假定客户端已升级。
5. 错误码沿用既有字面量；不得新造同义码或随意改客户端依赖的码。
6. 旧 `250 Send/Ack/Read/Pull` 不再兼容；新客户端必须使用 `246/245/210`。

## 11. 相关文档

- 系统总览：[architecture.md](architecture.md)
- 单聊可靠性：[design/messaging.md](design/messaging.md)
- 群聊 v3：[design/group-chat.md](design/group-chat.md)
- 网关与多实例：[design/gateway.md](design/gateway.md)
- 数据模型：[implementation/schema.md](implementation/schema.md)
- 功能状态：[implementation/status.md](implementation/status.md)
