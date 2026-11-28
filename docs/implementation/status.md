# 功能状态矩阵

> 状态：现状  
> 权威性：本文是「功能是否已实现 / 部分实现 / 未实现」的唯一来源  
> 最后核对：2026-11-26 @ `03ed7c2` + 工作区多网关/群聊改造  
> 适用代码：全仓库  
> 维护：功能状态变化时只改本文；其他文档只链接，不复制状态表。

## 图例

| 标记 | 含义 |
|---|---|
| ✅ | 已实现且主链路可用 |
| 🟡 | 部分实现：有可用主链路，缺规则/分支/指标 |
| ⬜ | 未实现：协议占位、stub 或明确不支持 |
| `[待清理]` | 已被新实现取代，旧代码/表/key 仍存在 |

## 1. 接入与账号

| 能力 | 状态 | 说明 |
|---|---|---|
| WSS/TLS 握手、二进制帧 | ✅ | `WebServer` + `WSSession` |
| 注册 `90` / 登录 `91` | ✅ | 登录与注册已拆分，不再复用 `101` |
| 心跳 `0` + 路由 CAS 续期 | ✅ | 续期失败发 `KICKED` 并断开 |
| 写队列 `queuedWrite` | ✅ | 认证后所有出站帧串行化，容量 256 |
| 连接上限 `GATEWAY_MAX_CONNECTIONS` | ✅ | 超限回 `SERVER_BUSY` |
| 优雅下线 `SERVER_DRAINING` | ✅ | 停注册 → 关 acceptor → 推帧 → grace → 强制关闭 |
| 同账号互踢 | ✅ | 同实例 + 跨实例 `KickUser` |
| UUID token + Redis | ✅ | TTL 7 天；当前不续期、不吊销 |
| 账号号段池 | ✅ | `account_pool` + 补货锁 |
| 密码慢哈希 | ⬜ | 当前单轮 `SHA-256(salt||password)`，无 KDF |
| 登录失败/发送频率限流 | ⬜ | 当前无 |
| 多端同时在线 | ⬜ | 当前互斥登录 |

## 2. 单聊

| 能力 | 状态 | 说明 |
|---|---|---|
| 发送 `101`、幂等 `clientMsgID` | ✅ | `im_outbox.dedupe_key = sendID:clientMsgID` |
| 每会话稠密 `seq` | ✅ | `im_conversation_seq`，事务内 +1；不再用 Snowflake |
| outbox 两阶段（stage/state/lease） | ✅ | `im_outbox`；worker 可重试 |
| Mongo 正文 + `committed` | ✅ | `IM-System.msg`，`serverMsgID` 唯一 |
| Kafka → PushService → 网关 | ✅ | `msg_topic`，key=convID |
| 在线推送 `104` | ✅ | `GatewayRouter.PushFrame` |
| 按会话 ACK `106` 水位 | ✅ | `im_conversation_receipts.delivered_seq`，无响应帧 |
| 离线同步 `200/209` | ✅ | 50 会话 × 每会话 20 条，不等 ACK |
| 历史 `244` | ✅ | `GetSingleChatHistory`，按 afterSeq 分页 |
| 混链拉取/按 seq 批量 | ⬜ | 旧接口已从 `msg.proto` 移除，仅保留 101/244 链路 |
| 撤回/删除/清空 | ⬜ | 协议有字段，未实现 |
| 富媒体 | ⬜ | 仅保留 `contentType` 枚举位 |
| Mongo TTL/归档 | ⬜ | 正文长期保留，容量无上界 |

## 3. 群聊（v3）

| 能力 | 状态 | 说明 |
|---|---|---|
| 群元数据/成员/申请/审批 | ✅ | `im_groups`、`im_group_members`、`im_group_applications` |
| 群发送 `246` | ✅ | 成员行锁 → `im_group_stream.next_seq+1` → staging + outbox(op=4) |
| 群内稠密 `seq` | ✅ | `im_group_stream.next_seq`；`published_seq` 为已发布水位 |
| 群消息 Mongo 单副本 | ✅ | `convID=groupID`、`isGroupMsg=true`、`recvID=0` |
| outbox `op=4` + staging | ✅ | worker 写 Mongo → `PublishMessage` → Kafka |
| Kafka `group_topic` 扇出 | ✅ | `GroupFanoutHandler`，key=groupID |
| 小群内联 `251` / 大群 hint | ✅ | ≤200 内联；>200 只带 `lastSeq/epoch` |
| 群历史 `245` | ✅ | Mongo 范围读，`afterSeq`/`beforeSeq`/`snapshot` |
| 群同步 `210` | ✅ | `GroupSyncSummary`；`needsPull=(toSeq<lastSeq)` |
| 群 ACK | ⬜ | 设计上不存在；未读由客户端本地维护 |
| 旧 `250 Send/Pull/Ack/Read` | `[待清理]` | 返回 `UNSUPPORTED_OPERATION`，改用 `246/245/210` |
| `im_group_messages` 旧表 | `[待清理]` | v3 不再读写；由 `InitializeGroups`/重置脚本清理 |
| 群管理员/转让/禁言/公告 | ⬜ | 仅群主模型 |
| tombstone / `deletedSeq` | ✅ | `mysqlhandler_group.cpp` 写 tombstone；`245` 返回 `deletedSeq` |

## 4. 好友与会话

| 能力 | 状态 | 说明 |
|---|---|---|
| 好友 6 个操作 `220–225` | ✅ | 查用户/申请/列表/申请列表/处理/删除 |
| 好友关系双行 + `friend_pairs` 行锁 | ✅ | `COUNT(*)==2` 才算好友 |
| 删除水位 `friend_chat_deletions` | ✅ | 历史按 `seq > deleted_seq` 可见 |
| 好友变更通知 `230` | ✅ | 在线推送；离线靠登录全量同步 |
| 好友黑名单/备注写入/上限/频控 | ⬜ | 未实现 |
| 会话列表 `240` | ✅ | 置顶/最近时间排序，隐藏不返回 |
| 标记已读 `241` | ✅ | `read_seq`、未读明细回减 |
| 置顶/免打扰 `242` | ✅ | `isPinned`、`recvMsgOpt` |
| 隐藏会话 `243` | ✅ | 软删；新消息到达复位 |
| 显式创建会话/清空会话 | ⬜ | 当前会话行由消息落库创建 |
| `min_seq`/清空语义 | 🟡 | 字段存在，写入点/语义需复核 |

## 5. 网关与多实例

| 能力 | 状态 | 说明 |
|---|---|---|
| 实例注册 `/services/{svc}/{instanceId}` | ✅ | JSON value，上报真实地址 |
| 路由表 `gw:user:{account}` | ✅ | TTL 300s，`instanceId:sessionId` |
| 心跳 CAS 续期 | ✅ | Lua；失败主动断开 |
| 跨实例互踢 `KickUser` | ✅ | 登录先踢旧持有者 |
| `GatewayRouter` 三态 | ✅ | `Delivered/NotOnline/Unknown` |
| 广播兜底 | ✅ | 路由失效/实例不可达时 |
| 群扇出 `PushBatchBestEffort` | ✅ | 群推送不做广播放大 |
| per-instance 指标 | 🟡 | 连接数/推送成功率/广播次数/被踢次数待补 |
| URL path/Origin 校验 | ⬜ | 当前不校验 |
| 服务端主动心跳 | ⬜ | 仅客户端 ping；静默断网在线状态最长滞留 300s |

## 6. 可观测与运维

| 能力 | 状态 | 说明 |
|---|---|---|
| Trace（网关→Msg→outbox→Kafka→Push→网关） | ✅ | 同步段 parent；异步投递段 link + `im_outbox.traceparent`；span 语义见 [design/observability.md](../design/observability.md) |
| Log（spdlog + OTel sink） | ✅ | `APE_LOG_*` 结构化字段；控制台/OTel 带 trace_id；热路径不再打印 payload |
| Metric | ✅ | 网关/消息/outbox/Kafka/推送/群扇出/RED；标签白名单见 observability.md |
| `SdkWSResp.logID` 回填 traceId | ✅ | 101/244/245/246 响应 + 104 推送帧；其他类型空串 |
| HTTP `/healthz` / gRPC health | 🟡 | gRPC health 已启用；网关 `/healthz`、`/readyz` 需显式开启 `--health-port` |
| 中间件配置外置 | 🟡 | 部分环境变量；大量硬编码 |
| 服务端镜像/编排 | ⬜ | 当前只有依赖服务编排 |
| CI | ⬜ | 仓库未发现 CI 配置 |
| 文档体系 | ✅ | 2026-11 重排为按用途分层 |

## 7. 已知风险与待清理

1. `MySQLHandler` 仍承载大量好友/群/会话业务规则，新增逻辑应放 service 层。
2. `im_group_messages`、旧 `250` 消息操作、`im_message_receipts`、`pending_ack:*` 等遗留物需确认清理。
3. 密码哈希、限流、令牌吊销、Mongo TTL 是上线前的主要缺口；健康检查已具备（gRPC health + 可选网关 HTTP），但未接入编排。
4. 多网关滚动升级必须保证新旧接口共存，或整包替换。
5. 当前工作区有未提交改造；不要用 `git reset --hard` / `git clean -fd` 清理。

## 8. 如何更新本文

1. 以当前代码/proto 为准核对。
2. 修改对应行的状态与说明；不要在这里抄设计细节。
3. 同步更新 `docs/README.md` 的权威性表（如果新增/废弃文档）。
4. R2 及以上变更，在 `implementation/records/` 留一条记录并链接。
