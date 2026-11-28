# ape-server 文档地图

> 状态：现状  
> 权威性：本文是「文档入口、状态图例、权威性顺序」的唯一来源  
> 最后核对：2026-11-26 @ `03ed7c2` + 工作区多网关/群聊改造  
> 适用代码：全仓库

本目录不再按「功能」堆放文档，而是按**文档用途 / 生命周期**分层。每份文档只回答一个问题；同一事实只允许有一个权威来源，其他文档只链接、不复述。

## 1. 先读哪几份

| 你的目标 | 阅读顺序 |
|---|---|
| 第一次接触项目 | [README](../README.md) → 本文 → [architecture.md](architecture.md) → [requirements.md](requirements.md) |
| 改协议 / 接客户端 | [protocol.md](protocol.md) → [design/gateway.md](design/gateway.md) → [design/messaging.md](design/messaging.md) |
| 改消息可靠性 | [design/messaging.md](design/messaging.md) → [design/group-chat.md](design/group-chat.md) → [implementation/schema.md](implementation/schema.md) |
| 查某个功能是否已实现 | [implementation/status.md](implementation/status.md) |
| 跑起来 / 排障 | [operations.md](operations.md) → 组件 README（[docker](../docker/README.md)、[proto](../proto/Readme.md)、[smoke](../scripts/smoke/README.md)） |
| 可观测 / 接监控 | [design/observability.md](design/observability.md) → [operations.md](operations.md) §7 |
| 做测试/验收 | [testing/README.md](testing/README.md) → [testing/messaging.md](testing/messaging.md) / [testing/group-chat.md](testing/group-chat.md) |
| 让 Agent 接手 | [agent.md](../agent.md) → 本文 → 任务对应的设计/状态文档 |
| 看历史方案/审计 | [archive/](archive/)（只读，不得作为现状） |

## 2. 状态图例

每份文档头部必须带四行：

```markdown
> 状态：现状 / 设计稿 / 已实施 / 历史（已废弃）
> 权威性：本文是「XXX」的唯一来源 / 已被 YYY 取代
> 最后核对：YYYY-MM-DD @ <commit 或工作区说明>
> 适用代码：路径或模块
```

| 状态 | 含义 | 能否作为现状引用 |
|---|---|---|
| **现状** | 与当前代码/proto 一致 | ✅ |
| **设计稿** | 计划中，尚未落地 | ❌ 只能作为方案 |
| **已实施** | 设计已落地；细节可能仍需补充 | ✅ 以代码为准 |
| **历史（已废弃）** | 旧方案/旧审计，保留追溯 | ❌ |

补充标记：✅ 已实现 / 🟡 部分实现 / ⬜ 未实现 / `[待清理]` / `[待补充]`。

## 3. 权威性顺序

发生冲突时，按以下顺序裁决：

1. **当前代码 + `proto/*.proto` + 运行时 schema**（最高）。
2. [implementation/status.md](implementation/status.md)：功能是否可用。
3. [protocol.md](protocol.md)：WebSocket / gRPC / 错误码契约。
4. [implementation/schema.md](implementation/schema.md)：表、索引、Redis key、Kafka topic。
5. [design/](design/)：为什么这样设计、代价是什么。
6. [implementation/records/](implementation/records/)：某次改造实际做了什么、与方案的偏差。
7. [requirements.md](requirements.md)：产品要什么、边界在哪。
8. [archive/](archive/)：历史材料，最低权威性。

## 4. 目录结构

```text
docs/
├── README.md                 # 本文：文档地图与规则
├── architecture.md           # 系统总览：进程/端口/分层/消息链路/并发
├── protocol.md               # WS type 码、错误码、超时、兼容（前后端唯一契约）
├── requirements.md           # 产品范围、角色、场景、非功能需求、验收
├── design/
│   ├── messaging.md          # 单聊可靠性：outbox/seq/ACK/离线同步
│   ├── gateway.md            # 网关、写队列、多实例、路由、互踢、优雅下线
│   ├── group-chat.md         # 群聊 v3：单副本/stream/op=4/Kafka 扇出/210/245/246
│   ├── social.md             # 账号、好友、会话
│   └── observability.md      # 可观测：trace 传播/指标/日志/告警/排障
├── implementation/
│   ├── status.md             # 唯一功能状态矩阵
│   ├── schema.md             # 表/索引/Redis key/Kafka topic 现状
│   ├── backlog.md            # P0/P1/P2 待办
│   └── records/
│       └── 2026-11-multi-gateway.md
├── testing/
│   ├── README.md             # 测试策略、环境、命令入口
│   ├── messaging.md          # 单聊/ACK 用例
│   ├── group-chat.md         # 群聊用例
│   └── results/              # 测试结果 JSON
├── operations.md             # 本地开发、部署、可观测、排障
├── notes/
│   └── interview-notes.md    # 非权威：技术选型/面试复盘
└── archive/                  # 历史、废弃、只读
    ├── 2026-11-gateway-auth-audit.md
    ├── 2026-11-group-delivery-plan-v2.md
    └── legacy-requirements-v1.md
```

## 5. 单一事实源

| 事实 | 唯一来源 | 其他文档的正确做法 |
|---|---|---|
| 功能是否可用 | [implementation/status.md](implementation/status.md) | 只链接，不复制状态表 |
| WS type / 错误码 / 超时 | [protocol.md](protocol.md) | 引用章节，不重抄码表 |
| 进程、端口、启动顺序 | [architecture.md](architecture.md) | `operations.md` 只写操作步骤 |
| 表/索引/Redis key/Kafka topic | [implementation/schema.md](implementation/schema.md) | 迁移记录只写「改了什么」 |
| 设计理由与取舍 | [design/](design/) | 实施记录只写「实际怎么做」 |
| 可观测语义（span/指标/日志/告警） | [design/observability.md](design/observability.md) | status/operations 只链接，不复制指标表 |
| 测试用例与结果 | [testing/](testing/) | 设计文档不复制用例 |
| Agent 工作规则 | [agent.md](../agent.md) | 不重复项目事实，只给链接 |
| 历史审计 / 旧方案 | [archive/](archive/) | 明确标「已废弃」 |

## 6. 维护规则

1. **新增文档必须带四行状态头**；否则视为草稿，不得放在 `design/` 或 `implementation/`。
2. **行为/协议/schema 变化**时，同一次变更至少更新：
   - 功能状态：`implementation/status.md`
   - 协议：`protocol.md`（如果客户端可见）
   - 数据：`implementation/schema.md`（如果表/key/topic 变化）
   - 设计：对应 `design/*.md`
   - 记录：`implementation/records/`（R2 及以上）
3. **一个事实只写一次**；需要引用时用相对链接，不复制大段内容。
4. 文档超过约 300 行时，考虑按「架构 / 决策 / 协议 / 数据 / 测试」拆分。
5. 文档超过 30 天未核对，且期间代码有变更时，状态头应标「待复核」，不得继续标「现状」。
6. 旧文档不直接删除：先移入 `archive/`，在状态头写「已被 X 取代」，保留追溯。
7. 组件 README（`docker/`、`proto/`、`scripts/smoke/`）保留在原位；`operations.md` 负责链接，不复制命令。

## 7. 归档策略

- 已完成的改造：设计文档留在 `design/`，实施记录进 `implementation/records/`。
- 被取代的方案：进 `archive/`，文件名带日期/主题。
- 历史审计：进 `archive/`，明确标「行号与结论可能已过期」。
- 面试/个人笔记：进 `notes/`，明确标「非权威，不作为项目事实」。
- 归档文档不再更新；如需恢复为现状，先新建文档并重新核对代码。
