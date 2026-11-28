# 产品与系统需求

> 状态：需求基线（长期）  
> 权威性：本文是「产品范围、角色、场景、非功能需求、验收目标」的唯一来源  
> 最后核对：2026-11-26 @ `03ed7c2` + 工作区多网关/群聊改造  
> 适用代码：全仓库  
> 说明：功能是否已实现，以 [implementation/status.md](implementation/status.md) 为准；协议细节以 [protocol.md](protocol.md) 为准。

## 1. 定位与边界

ape-server 是面向桌面/移动 IM 客户端的**长连接即时通信后端**，承担：

1. **连接接入**：WSS 长连接、认证、心跳、写队列、连接上限、优雅下线。
2. **消息可靠性**：单聊「受理 → outbox → Mongo 落库 → Kafka → 网关推送 → ACK 水位」；群聊「单副本 → outbox → Kafka 扇出 → Pull 兜底」。
3. **社交关系**：账号注册/登录、好友申请与关系、群组与成员、会话与未读。
4. **基础设施**：etcd 注册发现、多网关路由、OpenTelemetry 可观测。

**做**：

- 手机号注册、10 位数字账号、密码登录、UUID token。
- 单聊文本消息、历史、离线同步、按会话 ACK、会话列表/已读/置顶/免打扰/隐藏。
- 好友 6 个操作；群创建/申请/审批/成员/改名/踢人/退群/解散；群消息发送/历史/同步。
- 多网关路由、跨实例互踢、广播兜底、优雅下线。
- OTel Trace / Log；Metric 部分覆盖。

**不做（当前版本）**：

- 富媒体（图片/语音/视频/文件/位置），仅保留 `contentType` 枚举位。
- 消息撤回、单条删除、按时间物理删除、清空会话正文。
- 群管理员/转让群主/禁言/公告/成员名片。
- 好友备注写入、黑名单、好友置顶。
- 多端同时在线（当前互斥登录）。
- APNs/FCM/厂商推送、离线推送 SDK。
- 服务端群未读/已读状态（群聊未读由客户端本地维护）。

## 2. 角色

| 角色 | 说明 | 主要能力 |
|---|---|---|
| 未认证连接 | 已建立 WSS、未通过 `91` 登录 | 仅注册（90）、登录（91） |
| 已认证用户 | 持有有效 token 的单聊用户 | 单聊、历史、好友、会话、群聊 |
| 群主 | `im_groups.owner` | 群审批、改名、踢人、解散 |
| 群成员 | `im_group_members.active=1` | 群消息、历史、退群、拉成员 |
| 网关（内部） | WSS 接入进程 | 认证、协议分发、在线推送、离线同步、路由 |
| 微服务（内部） | 8 个 gRPC 服务 | 各自领域逻辑 |

## 3. 典型场景

1. 新用户注册 → 登录拿 token → 建立 WSS 长连接。
2. A、B 建立好友关系；A 发单聊消息，B 在线即时收到并 ACK；B 离线则下次登录按会话 cursor 补投。
3. A 创建群，B 申请加入，群主审批；群内发消息，在线成员收 `251`，大群/漏推用 `245` 拉取。
4. B 登录/重连：先收单聊 `200/209`，再收群 `251/210`；客户端按 `serverMsgID` 去重、按 `seq` 排序。
5. 用户删除好友：双方关系删除，历史按删除水位不可见，会话清理。
6. 同一账号在另一网关登录：旧连接收到 `KICKED`；路由 CAS 保证只有一个持有者。

## 4. 功能需求（按域）

> 具体协议、状态与设计分别见 [protocol.md](protocol.md)、[implementation/status.md](implementation/status.md)、[design/](design/)。

| 域 | 需求 | 设计/状态 |
|---|---|---|
| 接入 | WSS 握手、90/91 认证、心跳、写队列、连接上限、优雅下线 | [design/gateway.md](design/gateway.md) |
| 账号 | 手机号注册、10 位账号、密码登录、token 校验、账号池 | [design/social.md](design/social.md) |
| 单聊 | 发送幂等、每会话稠密 seq、outbox、离线同步、按会话 ACK、历史 | [design/messaging.md](design/messaging.md) |
| 群聊 | 群元数据/成员、`246` 发送、`245` 历史、`210` 同步、`251` 扇出、无 ACK | [design/group-chat.md](design/group-chat.md) |
| 好友 | 查用户、申请、列表、申请列表、处理、删除、变更通知 | [design/social.md](design/social.md) |
| 会话 | 列表、标记已读、置顶/免打扰、隐藏 | [design/social.md](design/social.md) |
| 多网关 | 实例注册、路由表、CAS 续期、跨实例互踢、广播兜底、优雅下线 | [design/gateway.md](design/gateway.md) |
| 可观测 | Trace/Log/Metric、traceId 排障 | [operations.md](operations.md) |

## 5. 非功能需求

### 5.1 性能与容量

| 指标 | 目标 |
|---|---|
| 单实例长连接 | ≥ 5,000（4 worker，每连接 1 strand） |
| 单聊在线端到端 P99 | ≤ 200ms |
| 消息受理延迟 P99 | ≤ 50ms（不含落库） |
| 历史拉取 | 100 条/页，P99 ≤ 100ms |
| 会话列表 | 单用户 ≤ 2,000 会话，P99 ≤ 50ms |
| 群规模 | 小群 ≤200 内联；大群 >200 只 hint + Pull |
| 批量上限 | 单聊单批建议 ≤20；ACK 单帧 ≤100 会话 |

### 5.2 可用性与容错

| 故障 | 要求 |
|---|---|
| Redis 不可用 | 在线/路由失败不得当离线；登录/注册返回明确错误 |
| Mongo 不可用 | 消息受理仍可进 outbox；投递延后；历史明确报错 |
| MySQL 不可用 | 写操作失败并回 `DB_ERROR`，不得部分成功 |
| Kafka 不可用 | outbox 保持重试；消息不丢 |
| 下游服务不可用 | 网关回明确错误码，连接保持；不得静默黑洞 |
| 进程重启 | 未 ACK 消息靠 Mongo + 水位补投；outbox 靠租约回收 |
| 网关重启 | 客户端应重连；多网关时切其他实例 |

### 5.3 安全

- 传输 TLS 1.2+；生产使用正式证书。
- 身份认证 token + `account` 绑定；`sendID`/`actor` 由服务端覆写。
- 密码必须使用慢哈希（Argon2id/bcrypt/PBKDF2）+ 随机盐；当前单轮 SHA-256 属于已知缺口。
- 凭据比较使用恒定时间比较；当前 `!=` 属于已知缺口。
- 令牌随机、可吊销、可滑动过期；当前无吊销/续期。
- 限流：登录失败、发送频率、好友申请；当前无。
- 日志不得输出 token、密码、完整消息正文、手机号。

### 5.4 可维护性

- 业务逻辑应在 service 层；不得继续把好友/群/会话规则堆进 `MySQLHandler`。
- 所有中间件连接参数外置化；当前大量硬编码。
- 协议变更必须双向回归（服务端 + 客户端）。
- 关键链路必须有 trace/日志/metric。

### 5.5 部署与配置

- 依赖通过 `docker/{etcd,kafka,redis,mongodb,mysql,otel}` 编排。
- 服务端自身镜像/编排当前缺失。
- 证书：开发用 `scripts/certs/` 自签；生产证书来源待补。
- 配置：当前仅少量环境变量；完整清单见 [operations.md](operations.md)。

## 6. 验收目标

### 6.1 功能验收

| 编号 | 用例 | 期望 |
|---|---|---|
| A-01 | 注册 13800000001 / 密码 / 昵称 | 返回 10 位账号；同 requestId 重试返回同一账号 |
| A-02 | 登录并建立 WSS | 返回 token；token 登录成功且 account 一致 |
| A-03 | 错误密码 | `401`，连接关闭，客户端统一提示 |
| A-04 | A、B 建立好友 | 申请 → 230 通知 → 同意 → 双方列表含对方 |
| A-05 | A 给在线 B 发消息 | B 收 `104`；B 回 `106`；outbox 完成 |
| A-06 | A 给离线 B 发消息 | 消息进 Mongo；B 登录收 `200`，最后收 `209` |
| A-07 | 重复投递同一条 | Mongo 仅一条；客户端仅落一条 |
| A-08 | 同 clientMsgID 重发不同内容 | `INVALID_MESSAGE`，不产生第二条 |
| A-09 | 历史 afterSeq 分页 | limit/hasMore/nextSeq 正确，无重复缺失 |
| A-10 | 标记已读 seq=5 | read_seq=5，未读减少，重复幂等 |
| A-11 | 隐藏会话后收新消息 | 会话重新出现 |
| A-12 | 删除好友 | 双方不可见；历史不可拉；清理任务执行 |
| A-13 | 建群 → 申请 → 审批 → 246 发消息 | seq 群内连续；在线成员收 `251`；`210` 同步完成 |
| A-14 | 踢人/退群/解散 | 权限正确；`active=0` 后不能发；解散后 `GROUP_DISMISSED` |
| A-15 | 群命令 clientID 重放 | 返回上次结果，不重复执行 |
| A-16 | 心跳 | `pong`；`gw:user:{account}` TTL 续期 |
| A-17 | 两个网关同账号登录 | 旧连接收 `KICKED`；路由指向新实例 |

### 6.2 非功能验收

| 编号 | 项 | 期望 |
|---|---|---|
| B-01 | 5,000 连接心跳 | CPU/内存平稳，无堆积 |
| B-02 | 单聊在线 P99 | ≤200ms |
| B-03 | kill -9 重启 | 未 ACK 消息补投，无丢失/重复落库 |
| B-04 | etcd 短暂不可用 | 已注册服务可用；恢复后 KeepAlive 正常 |
| B-05 | 日志含 traceId | 可串起网关→Msg→Push→GatewayPush |
| B-06 | 越权测试 | 伪造 `sendID`/`actor` 无法生效 |
| B-07 | 群聊 210/245 | 登录收 210；大群 needsPull；245 分页不丢不重 |

## 7. 兼容与演进

1. `proto/*.proto` 是契约源；客户端仓库必须同步并重新生成。
2. 字段只增不改号；废弃字段 `reserved`。
3. 新增 WS type 必须登记到 [protocol.md](protocol.md) 并给客户端最低版本。
4. 群聊 v3 是破坏性切换：旧 `250 Send/Ack/Read/Pull` 不再兼容；旧客户端必须升级。
5. 错误码沿用既有字面量，不得随意改客户端已依赖的码。
6. 多网关滚动升级必须保证新旧接口共存，或整包替换。

## 8. 相关文档

- 文档地图：[docs/README.md](README.md)
- 系统架构：[architecture.md](architecture.md)
- 协议：[protocol.md](protocol.md)
- 功能状态：[implementation/status.md](implementation/status.md)
- 数据模型：[implementation/schema.md](implementation/schema.md)
- 单独设计：[design/](design/)
- 运维：[operations.md](operations.md)
