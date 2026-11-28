# 网关与多实例设计

> 状态：已实施  
> 权威性：本文是「网关、写队列、多实例路由、跨实例互踢、优雅下线」的唯一设计来源  
> 最后核对：2026-11-26 @ `03ed7c2` + 工作区改造  
> 适用代码：src/gateway/、src/services/gateway_router.cpp、src/services/gateway_push_service/

> 方案已实施；实际落点与偏差见 [../implementation/records/2026-11-multi-gateway.md](../implementation/records/2026-11-multi-gateway.md)。

> 注意：本文部分离线同步/ACK 细节写于 ACK 水位改造前；当前单聊 ACK 以 [messaging.md](messaging.md) 为准，`im_message_receipts` 已由 `im_conversation_receipts` 取代。

| 项目 | 内容 |
|---|---|
| 文档版本 | v1.0 |
| 编写日期 | 2026-11-21 |
| 目标读者 | 服务端开发、客户端开发、运维 |
| 关联文档 | `docs/requirements.md`、`docs/implementation/backlog.md` |
| 增补章节 | **第 9 章**：决策记录（已确认）；**第 10 章**：离线消息同步与补投（按会话模型，含 PR-8） |
| 一句话目标 | 把"网关只能有一个实例"变成"可以有 N 个实例，且任意一个挂掉不影响其他人" |

> 本文中的行号以当前工作副本为准，改动后会有漂移；检索时优先用函数名。

---

## 0. 前提与结论摘要

### 0.1 先定三个决定

本方案按下列假设编写。若假设改变，第 4 章相应小节需要重做。

| # | 决定 | 本方案取值 | 若改成另一种 |
|---|---|---|---|
| **D1** | 同账号多端 | **互斥**：新登录踢掉旧的 | 多端在线：路由表要从"单值"改为"每端一条"，推送要扇出到所有端 |
| **D2** | 路由表失效时 | **广播兜底**（向所有网关各问一次） | 只等重登：实现更简单，但路由表变成正确性单点 |
| **D3** | 网关实例数上限 | **≤ 10** | 超过 20 个时广播成本不可接受，必须改为一致性哈希 + 可靠路由表 |

> 其余工程决策（实例 ID、路由 TTL、广播超时、兼容策略、同步粒度与规模等）见 **第 9 章 决策记录**。

### 0.2 结论摘要

改造分三层，缺一层都不成立：

1. **接口层**：`GatewayPushService` 从"只能推 104"泛化为"能推任意帧"（`PushFrame` / `PushBatch` / `IsOnline`）。这是现在三处"直调内存"的根因。
2. **路由层**：新增 `gw:user:{account}` 路由表 + 跨实例互踢，让"推给某个用户"能找到对的那台网关。
3. **判定层**：把"用户是否在线"的判据从 Redis 布尔值，改为目标网关的返回值；并且必须区分**确定不在线**与**没问到**。

第 3 条是最容易漏的：如果不区分，网关不可达时消息会被确认成"已分发"，然后永远消失。

---

## 1. 目标与非目标

### 1.1 目标

| 编号 | 目标 |
|---|---|
| G-1 | 网关可水平扩展到 N 个实例，共享同一批下游服务 |
| G-2 | 任一网关实例崩溃/重启，只影响该实例上的连接；客户端能自动恢复 |
| G-3 | 支持滚动发布（先摘流量、再关闭、连接迁移） |
| G-4 | 群消息、单聊消息、延迟回执三类推送在多实例下都能正确投递 |
| G-5 | 路由信息失效时，投递正确性不受影响（只影响性能） |

### 1.2 非目标

| 编号 | 不做 |
|---|---|
| N-1 | 同账号多端在线（D1 取互斥） |
| N-2 | 跨机房部署（实例数 ≤ 10，单机房） |
| N-3 | 消息在网关间迁移（断线即回登录补投，不做连接迁移） |
| N-4 | 网关侧无状态化（会话状态仍在进程内存，这是设计前提） |

---

## 2. 现状缺陷（为什么必须改）

| # | 缺陷 | 位置 | 单进程时 | 多进程/多实例时 |
|---|---|---|---|---|
| **C-1** | `PushToUser` 把帧类型写死成 104 | `gateway_push_service/server.cpp:34` | 掩盖问题 | 群通知（251）、延迟回执（101）**无路可走** |
| **C-2** | 三个非网关服务直接调 `WSSessionManager` | `group_service/server.cpp:44-51`、`66-84`；`outbox_worker.cpp:35` | 正常 | 各自进程内是空表 → **静默不投递**，无报错 |
| **C-3** | PushService 用 Redis 布尔值当投递闸门 | `push_service/server.cpp:123-134` | 偶发丢 | 写失败/竞态/负缓存 → 在线用户收不到消息且被确认"已分发" |
| **C-4** | etcd 注册按服务名而非实例 | `backbon_service/server.cpp` 写 `/services/{name}` | 无所谓 | 第二个实例**覆盖**第一个 |
| **C-5** | 注册地址是 `0.0.0.0:PORT` | `base_service.h` | 本机可用 | 跨主机不可路由 |
| **C-6** | `WSSessionManager` 是进程内单例 | `ws_session_manager.cpp:7-9` | 全局唯一真相 | 每实例只知道自己的连接 |
| **C-7** | 同账号重复登录只覆盖 map，不关旧连接 | `ws_session_manager.cpp:19-31` | 僵尸连接 | 跨实例时两个连接都"活着" |
| **C-8** | PushService 硬编码网关地址 | `push_service/server.cpp:211` | 本机可用 | 多机必挂 |
| **C-9** | `main.cpp` 单入口启动全部服务 | `src/main.cpp` | 能用 | 无法按进程部署 |

C-2 最危险：**不会崩、不会抛、不打 error 日志**，只是不工作。

---

## 3. 目标架构

### 3.1 部署形态

```
                    ┌──────────────────────────────────────┐
                    │            etcd  /services/*          │
                    └───────▲──────────────────▲───────────┘
                            │ 注册(带实例ID)     │ 订阅
   ┌────────────────────────┴──┐        ┌───────┴──────────────┐
   │  网关实例 gw-1            │        │  网关实例 gw-2        │
   │  ├─ WebServer   :6666     │        │  ├─ WebServer   :6667 │
   │  ├─ GatewayPushServer :50055       │  ├─ GatewayPushServer :50056
   │  └─ WSSessionManager      │        │  └─ WSSessionManager  │
   │     （本实例连接，权威）  │        │     （本实例连接）    │
   └────────────▲──────────────┘        └────────────▲─────────┘
                │  PushFrame/PushBatch/KickUser/IsOnline
                │  （按 account 路由到具体实例）
   ┌────────────┴───────────────────────────────────────────────┐
   │  PushService  │  GroupService  │  MsgService(OutboxWorker)  │
   └────────────────────────────────────────────────────────────┘
                │
   ┌────────────┴───────────────────────────────────────────────┐
   │  Redis  gw:user:{account} = instanceId:sessionId  (路由表)  │
   └────────────────────────────────────────────────────────────┘
```

### 3.2 职责边界

| 组件 | 职责 | 不做什么 |
|---|---|---|
| 网关 | 持有连接、讲 WebSocket 协议、执行推送、回答"这个人在不在我这儿" | 不判断好友关系、不读消息正文做业务校验 |
| `GatewayPushService` | 网关的 RPC 门面；**必须与网关同进程** | 不持有任何状态（状态在 `WSSessionManager`） |
| `GatewayRouter`（新增，客户端库） | 解析 account → 实例、调 RPC、广播兜底、三态归一 | 不做投递判定（判定在网关） |
| PushService | 决定"这条消息该不该推" | 不再用 Redis 判断在线 |
| GroupService | 群消息扇出 | 不再遍历"在线用户列表" |

### 3.3 必须守住的不变量

评审代码时逐条对照：

| 编号 | 不变量 | 违反后果 |
|---|---|---|
| **INV-1** | 只有持有该 account 活跃 session 的网关才能返回 `success=true` | 消息投到黑洞 |
| **INV-2** | `NotOnline` 只能由**所有**实例明确回复"我没有"才成立 | 网关不可达被误判为离线 → 消息丢失 |
| **INV-3** | 路由表只影响性能，不影响正确性 | 路由表成为正确性单点 |
| **INV-4** | 一个 account 同一时刻至多一个活跃 session（D1=互斥） | 双端收到/双端都收不到 |
| **INV-5** | 非网关进程不得直接访问 `WSSessionManager` | 静默失效 |
| **INV-6** | 路由续期只能由当前持有者成功（CAS） | 旧连接把路由改回自己 |

---

## 4. 关键设计

### 4.1 注册发现：服务名 + 实例 ID

**etcd key**：`/services/{ServiceName}/{instanceId}` → value = 可路由地址 `ip:port`

**实例 ID**：环境变量 `SERVICE_INSTANCE_ID`注入，缺省 `hostname:port`。要求**重启后保持不变**，否则路由表里的旧值会指向不存在的实例（由 TTL 与广播兜底消化）。

**proto 变更**（`proto/backbon.proto`）：

```proto
message RegisterServiceReq {
  string service = 1;
  repeated string ipport = 2;      // 原字段号不变
  repeated string methods = 3;     // 原字段号不变
  string instance_id = 4;          // 新增（必填）
}

message ServiceInstance {
  string instance_id = 1;
  string address = 2;
}

message GetServiceResp {
  bool registered = 1;
  string service = 2;
  repeated string ipport = 3;                 // 保留，兼容旧调用方
  repeated ServiceInstance instances = 5;     // 新增
  repeated string methods = 4;
}
```

> 以上全部是**追加字段**，不改任何已有字段号，新旧版本可混跑：旧调用方继续读 `ipport`，新调用方读 `instances`。等所有调用方迁移完成后再删 `ipport`（`reserved 3`）。

**注册地址**必须是真实 IP（`bind` 到 `0.0.0.0` 可以，但**上报**的必须是本机可路由地址，由配置或 `getifaddrs` 得出）。

**删除**：Redis 里 `/services/{service}` 的 methods 镜像（TTL 60s 且无刷新，过期后查不到 methods）。methods 直接放进 etcd value 的 JSON。

### 4.2 路由表

**key**：`gw:user:{account}`
**value**：`"{instanceId}:{sessionId}"`（`sessionId` = 每连接一个 UUID）
**TTL**：300s

| 时机 | 操作 |
|---|---|
| 认证成功 | `SET key "inst:sess" EX 300`（先按 4.3 处理旧持有者） |
| 每次心跳 | Lua CAS 续期 |
| 断线 | **不删**，靠 TTL 过期 |
| 被踢 | 被踢方不写（它已失去所有权） |

**续期脚本**（`EVAL`，返回值语义：1=续期成功，0=已失去所有权）：

```lua
local v = redis.call('GET', KEYS[1])
if v == ARGV[1] then
  return redis.call('EXPIRE', KEYS[1], tonumber(ARGV[2]))
end
return 0
```

返回 0 时，网关应：发 `type:0, errorCode:"KICKED"` → `onDisconnect()`。**这是跨实例互踢的兜底**，最坏延迟一个心跳周期（30s）。

**为什么用 CAS 而不是普通 SET/EXPIRE**：用户先在 gw-1 登录（连接 A），再在 gw-2 登录（连接 B）。A 的心跳若用普通 `EXPIRE`，会一直把路由续在自己名下，而消息会继续投给 gw-1 上的 A —— B 永远收不到。CAS 让 A 的续期失败并自杀。

**为什么断线不删**：删也有可能失败（Redis 抖动），而 TTL 是唯一可靠的清理。多留 300s 的无害旧值，会被广播兜底吸收。

### 4.3 跨实例互踢

只有 TTL + CAS 不够：在 30s 的窗口里，用户会在两个实例上各有一个"活着"的连接，且旧的那端仍在收心跳。

**新增 RPC**（`proto/gateway_push.proto`）：

```proto
rpc KickUser(KickUserReq) returns (KickUserResp);

message KickUserReq {
  uint64 account = 1;
  string reason = 2;            // "duplicate_login" / "admin" / ...
  string except_instance_id = 3; // 发起方自己，避免误踢
}
message KickUserResp { bool kicked = 1; }
```

**登录时的顺序**（`ws_session.cpp` 认证成功段）：

```
1. 读 gw:user:{account}
2. 若存在且 instanceId != 自己：
     best-effort 调该实例 KickUser(account, "duplicate_login", self)   // 失败可忽略
3. SET gw:user:{account} = "self:sess" EX 300                          // 无条件覆盖
4. 本地 registerSession（同实例内重复登录也要踢旧的，见 4.7）
```

Kick 失败不阻塞登录——因为第 3 步已经把路由切过来了，旧连接再也不会收到新消息，且它的下一次心跳续期会失败并自杀。

**被踢方的行为**：

```cpp
// 在 ws_.get_executor() 上投递
sdkws::SdkWSResp k;
k.set_type(0);
k.set_account(account_);
k.set_errorcode("KICKED");
k.set_errormsg("account logged in elsewhere");
queuedWrite(...);   // 尽力发送
co_await onDisconnect();
```

### 4.4 推送接口泛化

`proto/gateway_push.proto`：

```proto
service GatewayPushService {
  // 通用帧推送：type 由调用方决定（104 在线消息 / 251 群通知 / 101 延迟回执 / ...）
  rpc PushFrame(PushFrameReq) returns (PushFrameResp);
  // 批量推送：一次推一批账号（群扇出用），返回每个账号的结果
  rpc PushBatch(PushBatchReq) returns (PushBatchResp);
  // 在线查询
  rpc IsOnline(IsOnlineReq) returns (IsOnlineResp);
  // 踢下线
  rpc KickUser(KickUserReq) returns (KickUserResp);
}

message PushFrameReq {
  uint64 account = 1;
  int32  type    = 2;
  bytes  data    = 3;
}
message PushFrameResp {
  bool   success    = 1;   // true = 该 account 的 session 在本实例且已入队
  string instance_id = 2;  // 应答方实例，便于排障
}

message PushBatchReq {
  repeated uint64 accounts = 1;
  int32  type = 2;
  bytes  data = 3;
}
message PushBatchResp {
  repeated bool   results     = 1;  // 与 accounts 一一对应
  string          instance_id = 2;
}

message IsOnlineReq  { uint64 account = 1; }
message IsOnlineResp { bool online = 1; string instance_id = 2; }
```

**网关实现要点**：

```cpp
// gateway_push_service/server.cpp
sdkws::SdkWSResp frame;
frame.set_type(request->type());
frame.set_account(account);
frame.set_data(request->data());
bool pushed = WSSessionManager::instance().pushToUser(
    account, std::make_shared<std::string>(frame.SerializeAsString()));
response->set_success(pushed);
```

`success=true` 的语义严格限定为：**目标账号的 session 在本实例，且帧已入写队列**。它不代表客户端已收到、不代表落库——那是 ACK 的职责。

### 4.5 `GatewayRouter`（新增共享组件）

位置建议：`include/services/gateway_router.h` + `src/services/gateway_router.cpp`。约 100–150 行，不是框架。

**对外接口**：

```cpp
enum class PushResult {
  Delivered,   // 目标网关确认该用户在此实例且已入队
  NotOnline,   // 所有实例都明确回复"没有"
  Unknown      // 有实例不可达/超时，且无人报成功
};

class GatewayRouter {
public:
  // 解析 account → 实例（读 gw:user，miss 返回空）
  std::optional<std::string> Resolve(uint64_t account);

  // 单播；内部做 resolve → 调用 → 广播兜底
  boost::asio::awaitable<PushResult>
  PushFrame(uint64_t account, int32_t type, std::string data);

  // 批量；按实例分组，每组一次 RPC
  boost::asio::awaitable<std::vector<PushResult>>
  PushBatch(const std::vector<uint64_t>& accounts, int32_t type, std::string data);

  // 实例地址缓存（订阅 etcd 前缀 /services/GatewayPushService/）
  void OnInstancesChanged(std::vector<ServiceInstance>);

private:
  boost::asio::awaitable<bool> PushToInstance(const std::string& instanceId,
                                              uint64_t account, int32_t type,
                                              const std::string& data);
};
```

**三态判定规则**（务必照此实现）：

```
已知目标实例 I = Resolve(account)
若 I 存在:
    r = PushToInstance(I, ...)
    r == true            -> Delivered
    r == RPC 失败/超时    -> 进入广播
若 I 不存在 或 广播阶段:
    results = 对所有实例并发 PushFrame
    任一 true                         -> Delivered
    全部 false 且 全部应答成功        -> NotOnline
    任一调用失败/超时，且无人 true     -> Unknown
```

**`Unknown` 的处理必须由调用方决定**：

| 调用方 | Delivered | NotOnline | Unknown |
|---|---|---|---|
| PushService（104） | 等 ACK | `ConfirmMessageDispatch`（走离线恢复） | **抛异常** → Kafka 不提交 offset → 重试 |
| GroupService（251） | 记成功 | 记入"最近离线"集合（30s） | 记日志，下个 tick 重试 |
| OutboxWorker（101） | 结束 | 结束（发送方下次登录可见） | 记日志，下个 tick 重试 |

**超时与截止**：单次 RPC **300ms**；广播整体截止 **500ms**（Q-3）。超时视为“未应答”，计入 `Unknown` 判定。

**缓存与失效**：`instanceId → address` 存本地 map，通过 etcd 前缀订阅更新；未知实例按"广播"处理，不要直接失败。

### 4.6 群投递循环反转

**现状**（`group_service/server.cpp:66-84`）：

```cpp
for (auto actor : WSSessionManager::instance().onlineAccounts())   // 独立进程里恒为空
    for (auto id : PendingGroupsForDelivery(actor))
        WithGroupDelivery(actor, id, [](notice){ pushToUser(actor, ...); });
```

**改造后**：

```
每 2s tick:
  1. groups = QueryGroupsWithPending(limit=200)      // 按群驱动，不再按用户
  2. for each group:
       notice = BuildDelivery(group)                 // 现有逻辑复用
       members = GroupMembers(group)                 // 只取 active=1
       results = Router.PushBatch(members, 251, noticeBin)
       for (member, r) in zip(members, results):
           if r == NotOnline:   offlineCache[member] = now + 30s
           if r == Delivered:   // ack_seq 仍由客户端 Ack 推进，这里不推进
  3. 从 members 中剔除 offlineCache 里未过期的成员，避免每 2s 重复推
```

**新增 SQL**（`mysqlhandler.cpp`）：`QueryGroupsWithPending(limit)` —— `SELECT g.id FROM im_groups g WHERE g.dismissed=0 AND g.last_seq > (SELECT MIN(ack_seq) FROM im_group_members WHERE group_id=g.id AND active=1) LIMIT ?`（需要 `im_group_members(group_id, ack_seq)` 或 `(group_id, account)` 上已有索引，见 `scripts/sql/group_chat.sql`）。

**为什么必须反转**：`onlineAccounts()` 是网关进程内状态，多进程下拿不到；而"按群驱动 + 网关过滤离线"天然分布式安全——网关对不属于自己的用户返回 false，不会投错。

**`offlineCache` 只允许做"减少重复推送"**，不允许做"决定不投递"。它的语义是"这个人 30 秒内别问了"，过期后必须重新尝试——因为群消息没有"确认分发"这个不可逆动作，客户端登录/定时 `Pull` 都能补上。

### 4.7 网关内部

| # | 改动 | 说明 |
|---|---|---|
| 4.7.1 | 每个 `WSSession` 生成 `sessionId_`（UUID） | 路由 CAS 与互踢比对用 |
| 4.7.2 | `registerSession` 覆盖时主动踢旧连接 | 修 C-7：发 KICKED → `onDisconnect()`，同实例内也走这条路径 |
| 4.7.3 | 心跳加路由 CAS 续期；失败即自杀 | 见 4.2 |
| 4.7.4 | 认证成功时写路由（含 4.3 的踢旧） | `ws_session.cpp` 认证成功段 |
| 4.7.5 | 删除 `user:{account}:online` 的全部写入 | `ws_session.cpp` 登录 / 心跳 / `onDisconnect`；`backbon_service/server.cpp` 负缓存 |
| 4.7.6 | `gateway_push_service` 注册 `instance_id`，上报真实地址 | 见 4.1 |

### 4.8 优雅下线

**`SIGTERM` 顺序**：

```
1. 从 etcd 注销 /services/GatewayPushService/{instanceId}
   （新登录不再被路由到本实例；PushService 的 Resolve 会指向别处或走广播）
2. 关闭 WSS acceptor（不再接受新连接）
3. 给所有本地 session 发 {type:0, errorCode:"SERVER_DRAINING"}
4. 停止路由续期（不再 CAS，避免占用路由）
5. 等待 grace（建议 30s）——让客户端完成重连
6. 强制关闭剩余 session（发 KICKED 或直接 close）
7. 停止 IOC pool、停止 gRPC、刷 OTel
```

**客户端收到 `SERVER_DRAINING` 应立刻重连**（不能等心跳超时），且**不应提示用户**——这是正常运维事件。

**注意顺序**：第 1 步必须早于第 2 步，否则"已注销但还在接新连接"会造成新连接分配到正在关闭的实例。

### 4.9 客户端配合（不做则多网关无收益）

多网关的价值是"任一台挂了其他人还能用"。但当前客户端**没有自动重连**（`webSocketSession.cpp` 只有 `close()` + `connectionLost`），网关一重启用户就得手动重登——多网关白做。

| # | 改动 | 位置（IM-client） |
|---|---|---|
| 4.9.1 | token 持久化（DPAPI 加密），供免密重连 | 新增 `src/storage/secureTokenStore.*` |
| 4.9.2 | 自动重连 + 退避（1/2/4/8/16/30s 封顶），用 token 走 `type=101` 的 token 分支 | `webSocketSession.cpp` |
| 4.9.3 | 处理 `KICKED`（回登录页 + 提示"账号在其他设备登录"） | `webSocketSession.cpp` / `applicationController.cpp` |
| 4.9.4 | 处理 `SERVER_DRAINING`（立即重连，不提示用户） | 同上 |
| 4.9.5 | 重连成功后重拉会话/好友/群，并触发离线补投 | `chatService.cpp` / `friendService.cpp` / `groupService.cpp` |
| 4.9.6 | 首帧填 `cursors`（每会话本地最大 seq） | `webSocketSession.cpp` `sendRequest` |
| 4.9.7 | 填充 `deviceID` | 同上（多端在线的前置，本方案不使用） |

**新增错误码**（属于 WS 协议扩展，需登记到 [../protocol.md](../protocol.md) §6）：

| errorCode | 语义 | 客户端行为 |
|---|---|---|
| `KICKED` | 账号在其他实例/设备登录 | 断开、回登录页、保留本地数据 |
| `SERVER_DRAINING` | 本网关正在下线 | 立即重连到其他实例，不打扰用户 |
| `SERVER_BUSY` | 本实例连接数超限 | 退避后重连 |

---

## 5. 改动清单（按模块）

### 5.1 proto

| 文件 | 改动 |
|---|---|
| `proto/backbon.proto` | `RegisterServiceReq` 加 `instance_id`；新增 `ServiceInstance`；`GetServiceResp` 改为 `repeated ServiceInstance` |
| `proto/gateway_push.proto` | 新增 `PushFrame` / `PushBatch` / `IsOnline` / `KickUser`；**删除** `PushToUser`（Q-4：不做兼容层） |
| `proto/sdkws.proto` | 无需改动（`errorCode` 是字符串，新码不需要改结构） |

重新生成：

```bash
./vcpkg_installed/x64-linux/tools/protobuf/protoc -I=./proto \
  --cpp_out=./src/proto --grpc_out=./src/proto \
  --plugin=protoc-gen-grpc=./vcpkg_installed/x64-linux/tools/grpc/grpc_cpp_plugin \
  ./proto/*.proto
```

### 5.2 服务端源码

| 文件 | 改动 | 关联 |
|---|---|---|
| `src/main.cpp` | 按 `--role` 拆分启动流程；每个 role 只初始化所需依赖 | 5.2.1 |
| `include/services/base_service.h` | 注册时带 `instance_id`；`EnableEtcdRegistration` 签名扩展 | 4.1 |
| `src/services/backbon_service/server.cpp` | etcd key 改 `/services/{svc}/{id}`；`GetService` 返回实例列表 | 4.1 |
| `src/services/gateway_push_service/server.cpp` | 实现 `PushFrame` / `PushBatch` / `IsOnline` / `KickUser`；`type` 不再写死 | 4.4 |
| `src/services/gateway_push_service/client.cpp` | 新增对应客户端方法 | 4.4 |
| `include/services/gateway_push_service/server.h` | 新 RPC 覆写 | 4.4 |
| **新增** `include/services/gateway_router.h` / `src/services/gateway_router.cpp` | 路由 + 广播 + 三态 | 4.5 |
| `src/services/push_service/server.cpp` | 删 Redis 闸门；改用 `Router.PushFrame`；`Unknown` 抛异常；地址走 etcd 异步初始化 | 4.5、C-3、C-8 |
| `src/services/group_service/server.cpp` | `Notify`/`Deliver` 改走 Router；`Deliver` 循环反转 + `PushBatch` + `offlineCache` | 4.6、C-2 |
| `src/services/msg_service/outbox_worker.cpp` | `notifySender` 改走 Router | C-2 |
| `src/storage/mysqlhandler.cpp` | 新增 `QueryGroupsWithPending`；`PendingGroupsForDelivery` 保留或删除 | 4.6 |
| `src/gateway/ws_session.cpp` | `sessionId_`；路由写入/CAS 续期；KICKED 帧；删 online key | 4.2、4.3、4.7 |
| `include/gateway/ws_session.h` | 新增 `sessionId_`、`handleDraining()` | 4.7 |
| `src/gateway/ws_session_manager.cpp` | `registerSession` 踢旧连接 | C-7 |
| `src/gateway/webserver.cpp` | 优雅下线序列；连接数上限 | 4.8 |

### 5.3 Redis key 变更

| key | 动作 |
|---|---|
| `user:{account}:online` | **删除**（4 个写入点 + 1 个读取点） |
| `gw:user:{account}` | **新增**，路由表，TTL 300s |
| `token:{token}` | 不变 |
| `msg:{serverMsgID}` | 不变 |
| `/services/{service}`（methods 镜像） | 删除，methods 并入 etcd value |

---

## 6. PR 拆分

每个 PR 可独立评审、独立回滚；依赖关系标注在"前置"列。

**验收环境约定**：下文命令假设 `etcdctl` / `redis-cli` / `mysql` / `kafka-consumer-groups.sh` 已在 PATH 中；若只在容器内可用，请加 `docker exec`（Kafka 客户端在 `apache/kafka:4.1.1` 的 `/opt/kafka/bin/` 下）。密码统一走环境变量（`MYSQL_PWD` / `REDISCLI_AUTH`），不要写在命令行上。

### PR-1（P0）进程拆分与实例化注册

| 项 | 内容 |
|---|---|
| 目标 | 服务可以按进程部署；每个实例注册到独立 etcd key |
| 改动 | `main.cpp` 按 role 启动；`base_service.h` 带 `instance_id`；`backbon.proto` + `backbon_service` 多实例；注册真实 IP；配置外置 |
| 前置 | 无 |
| 风险 | 中（启动路径全改）。保留 `--role=all` 兼容现有单进程开发模式 |
| 回滚 | `--role=all` 回到旧行为 |

**验收命令**：

```bash
# 0) 前置：基线可编（先确认，再改）
cmake --build build --target main -j

# 1) 起两个网关实例（不同端口、不同实例 ID）
SERVICE_INSTANCE_ID=gw-1 ./main --role=gateway --ws-port=6666 --push-port=50055 &
SERVICE_INSTANCE_ID=gw-2 ./main --role=gateway --ws-port=6667 --push-port=50056 &

# 2) 两个实例各占一个 key，且 key 里带实例 ID
etcdctl --endpoints=127.0.0.1:2379 get --prefix /services/GatewayPushService --keys-only
# 期望两行：/services/GatewayPushService/gw-1、/services/GatewayPushService/gw-2

# 3) 值必须是真实可路由地址（键值要对得上）
etcdctl --endpoints=127.0.0.1:2379 get --prefix /services/GatewayPushService
# 期望 gw-1 -> <本机IP>:50055、gw-2 -> <本机IP>:50056

# 4) 任何注册值都不能是 0.0.0.0
#    注意：grep 无匹配时退出码为 1，脚本里必须兜底，否则会误判失败
etcdctl --endpoints=127.0.0.1:2379 get --prefix /services/ | grep "0.0.0.0:" \
  && echo "FAIL: 0.0.0.0 registered" || echo "OK: no 0.0.0.0"

# 5) 旧单进程模式回归：能登录、能收发消息
./main --role=all &
```

### PR-2（P0）推送接口泛化 + 消除直调

| 项 | 内容 |
|---|---|
| 目标 | 非网关进程不再直接访问 `WSSessionManager`；旧 `PushToUser` 删除 |
| 改动 | `gateway_push.proto` + 网关实现 `PushFrame`/`PushBatch`/`IsOnline`；**删除** `PushToUser`；`GroupService::Notify`、`OutboxWorker::notifySender` 改走 gRPC；临时用配置地址（路由在 PR-3） |
| 前置 | PR-1 |
| 风险 | 中（行为等价替换，但跨进程后延迟从纳秒变亚毫秒）。**因删除旧接口，升级必须整包替换**（见 §8） |

**验收命令**：

```bash
# 1) 非网关进程不得访问 WSSessionManager
#    白名单只有：网关自身、网关 RPC 门面。src/main.cpp 不在白名单内。
grep -rn "WSSessionManager" src/ include/ \
  | grep -v "^src/gateway/" \
  | grep -v "^src/services/gateway_push_service/" \
  | grep -v "^include/services/gateway_push_service/" \
  || echo "OK: no cross-process access"

# 2) 旧接口已删除（Q-4）；只看手写源码，忽略生成物
grep -rn "PushToUser" proto/ src/ include/ | grep -v "\.pb\.\(cc\|h\)" \
  || echo "OK: PushToUser removed"

# 3) 群改名后，在线成员收到 type=251
#    GroupService 日志应出现 PushFrame(type=251)，客户端 UI 自动刷新

# 4) PENDING 之后的延迟 101 回执能到达发送方
#    人为放慢 outbox 让 SendMessages 返回 PENDING，确认客户端最终收到 type=101

# 5) 配置地址生效：把网关地址指向 gw-2，确认 104 落到 gw-2 上的用户
```

### PR-3（P1）路由表 + 跨实例互踢

| 项 | 内容 |
|---|---|
| 目标 | `account → instance` 可解析；同账号两地登录时旧的被踢 |
| 改动 | `gw:user:{account}` 写入/CAS 续期；`KickUser` RPC；登录流程加"读路由 → 踢旧 → 覆盖"；`registerSession` 踢旧（同实例） |
| 前置 | PR-1、PR-2 |
| 风险 | 高（涉及认证主链路）。加开关 `GATEWAY_ROUTING=0/1` 灰度 |

**验收命令**（以下都用测试账号，不要用真实账号）：

```bash
# 1) 登录后路由存在，格式为 inst:sess
REDISCLI_AUTH=redis123 redis-cli GET gw:user:1000000001     # 期望 "gw-1:<uuid>"

# 2) 同账号在 gw-2 登录 -> 路由被切走
REDISCLI_AUTH=redis123 redis-cli GET gw:user:1000000001     # 期望 "gw-2:<uuid>"

# 3) gw-1 上的旧连接收到 KICKED 并断开
#    客户端日志出现 errorCode=KICKED

# 4) CAS 自愈：篡改路由值，旧连接应在 ≤1 个心跳（30s）内自杀
REDISCLI_AUTH=redis123 redis-cli SET gw:user:1000000001 "gw-2:fake" EX 300
# 期望 gw-1 上该连接在一次续期失败后主动断开

# 5) 同实例重复登录（同一端口再连一次）也要踢掉前一个连接
```

### PR-4（P1）GatewayRouter 与三态判定

| 项 | 内容 |
|---|---|
| 目标 | 路由失效不影响正确性；网关不可达不被误判为离线 |
| 改动 | 新增 `GatewayRouter`；PushService / GroupService / OutboxWorker 全部改走它；广播兜底；`Unknown` 抛异常 |
| 前置 | PR-3 |
| 风险 | 高（引入广播）。超时按 **单次 300ms / 整体 500ms**（Q-3）；广播并发上限；先 resolve 再广播 |

**验收命令**：

```bash
# 1) 删路由 -> 广播兜底仍然送达
REDISCLI_AUTH=redis123 redis-cli DEL gw:user:1000000001
# 发消息应实时到达；服务端日志出现 broadcast fallback

# 2) 目标实例地址改成不可达 -> Unknown，不得被当成 NotOnline
etcdctl --endpoints=127.0.0.1:2379 put /services/GatewayPushService/gw-2 "127.0.0.1:1"
# 发消息给连在 gw-2 上的用户，然后看消费滞后
# 客户端在 apache/kafka:4.1.1 镜像的 /opt/kafka/bin/ 下；宿主机无此命令时用 docker exec
docker exec -it <kafka-container> /opt/kafka/bin/kafka-consumer-groups.sh \
  --bootstrap-server localhost:29092 --describe --group push-service-group | grep msg_topic
# 期望 LAG 增长（未提交）；恢复地址后 LAG 回落，消息补投且只投一次

# 3) 用户确实离线时走离线恢复并确认分发
MYSQL_PWD=ape_password mysql -h127.0.0.1 -uape_user ape_auth \
  -e "select id,state,dispatch_done,available_at from im_outbox order by id desc limit 5;"
# 期望 state=2, dispatch_done=1

# 4) Unknown 时不得确认分发
# 期望该条仍为 state=0/1、attempts 递增、last_error 非空

# 5) 广播成本上限：单条消息的 RPC 次数 ≤ 网关实例数（看日志计数）
```

### PR-5（P1）群投递反转

| 项 | 内容 |
|---|---|
| 目标 | 群消息在多进程/多实例下真正可投递 |
| 改动 | `QueryGroupsWithPending` / `GroupMembers`；`Deliver` 循环反转；`PushBatch`；`offlineCache`（30s TTL） |
| 前置 | PR-2、PR-4 |
| 风险 | 中（群扇出放大，注意 batch 上限与背压） |

**验收命令**：

```bash
# 1) 群成员分布在两个网关时，发群消息双方都收到 251
# 2) 200 人群一个 tick 只发 1 次 PushBatch，且群路径不再走 PushFrame
grep -c "PushBatch" <group_service.log>    # 应 ≈ tick 数 × 待投递群数
grep -c "PushFrame" <group_service.log>    # 群路径应为 0
# 3) 离线成员 30s 内不被重复推（观察 offlineCache 命中日志）
# 4) 小群 Notice 内联 20 条；大群 messages 为空、只带 lastSeq（Q-5）
# 5) 已解散（dismissed=1）的群不出现在待投递集合里
```

### PR-6（P2）优雅下线与可观测

| 项 | 内容 |
|---|---|
| 目标 | 滚动发布不断服；指标能看出是哪个实例出问题 |
| 改动 | `webserver.cpp` 下线序列；`SERVER_DRAINING`；连接数上限 `SERVER_BUSY`；每实例指标 |
| 前置 | PR-1 |
| 风险 | 低（失败只影响发布体验） |

**验收命令**：

```bash
# 1) 对 gw-1 发 SIGTERM -> 客户端收到 SERVER_DRAINING 并自动迁移到 gw-2
kill -TERM <gw-1-pid>

# 2) gw-1 的 etcd key 在 2s 内消失（第 1 步注销必须早于关 acceptor）
etcdctl --endpoints=127.0.0.1:2379 get --prefix /services/GatewayPushService --keys-only

# 3) grace 期内已建连接不立即断开，期满后统一关闭
# 4) 连接数上限：把上限临时调到 2，第 3 个连接应收到 SERVER_BUSY 并退避重连
# 5) OTel 指标带 instance_id 维度（按实例看连接数、推送成功率、广播次数、被踢次数）
```

### PR-7（P2）客户端重连（独立仓库）

| 项 | 内容 |
|---|---|
| 目标 | 网关重启/下线后客户端自动恢复 |
| 改动 | token 持久化、自动重连 + 退避、`KICKED`/`SERVER_DRAINING` 处理、重连后重拉会话/好友/群、首帧携带 `cursors`（每会话 `lastSeq`） |
| 前置 | 无（可与 PR-3～6 并行）；但 **PR-6 的收益完全依赖它** |

> 注意：客户端上报的是 `cursors`（每会话水位，Q-9B），不是旧的全局 `lastMsgSeq`。

**验收**：

```bash
# 1) 断网 10s 恢复 -> 自动重连成功，无需输入密码
# 2) 杀掉网关 -> 在退避窗口内重连到新实例
# 3) 同账号在另一端登录 -> 本端提示"账号在其他设备登录"并回登录页
# 4) 重连后会话列表 / 未读数 / 群消息与断线前一致
# 5) 首帧确实携带 cursors：服务端只补差量，不重复下发已收到的消息
# 6) 连续失败 N 次（N=5）后弹窗，用户可选「重试 / 退出登录」，不再无限重连
```

### PR-8（P1）离线消息同步改造

定义在 **第 10.11 节**（按会话同步替代穷举补投），验收命令也见 **§10.11**、矩阵见 **§10.12（V-12～V-18）**。前置 PR-2；建议在 PR-3/PR-4 之后实施，因为它依赖"先写路由、再同步"的时序。

---

## 7. 验收矩阵

| # | 场景 | 期望 | 依赖 |
|---|---|---|---|
| V-1 | A→gw-1，B→gw-2，互发单聊 | 双向实时，ACK 后 `im_outbox.state=2` | PR-2/3/4 |
| V-2 | A→gw-1 建群，成员分布在两网关 | 双方都收到 251 | PR-5 |
| V-3 | 同账号先 gw-1 后 gw-2 | gw-1 旧连接收 KICKED；消息只到 gw-2 | PR-3 |
| V-4 | 手工删路由后发消息 | 广播兜底命中，不丢消息 | PR-4 |
| V-5 | `kill -9` gw-2 | 用户重连 gw-1；期间消息补投，不丢不重 | PR-3～8 |
| V-6 | `SIGTERM` gw-1 | 客户端收 DRAINING 并自动迁移 | PR-6/7 |
| V-7 | 路由指向不可达实例 | `Unknown` → Kafka LAG 增长 → 恢复后补投；**不得**被确认为已分发 | PR-4 |
| V-8 | 单网关压测 | 得出连接数/消息量天花板 | 无（现在即可做） |
| V-9 | 回滚开关：`GATEWAY_ROUTING=0` | 路由不读写，投递改走广播，仍然不丢消息 | PR-3/4 |
| V-10 | 实例 ID 变更（换端口重启） | 旧路由在 TTL 内失效；期间消息由广播兜底送达，不丢 | PR-3/4 |
| V-11 | N 个实例时的一条消息 | 单条消息的 gRPC 调用次数 ≤ N（广播有上限，不放大） | PR-4 |

> V-12～V-21（离线同步相关）见 **§10.12**。

---

## 8. 风险与回滚

| 风险 | 影响 | 缓解 |
|---|---|---|
| 广播风暴（D2=广播） | 一条消息 × N 次 RPC | 实例数 ≤10；广播并发上限；先 resolve 再广播，命中就不广播 |
| 路由 CAS 脚本出错 | 旧连接自杀或新的收不到 | 灰度开关；脚本先在测试环境验证 |
| 踢旧逻辑误伤 | 用户被无故踢线 | `except_instance_id` 防自踢；`KickUser` 只在确认路由指向他人时调用 |
| 群扇出放大 | GroupService CPU 升高 | `PushBatch` + `offlineCache` + tick 内限流 |
| 客户端未升级 | 收不到 DRAINING / 不重连 | 服务端保持旧行为（不主动断开），靠滚动窗口；客户端最低版本约束 |
| proto 追加字段（`instance_id`、`instances`、`cursors`） | 新旧版本混跑 | 全部追加、不改字段号；`ipport` 保留至迁移完成 |
| **删除 `PushToUser`（Q-4）** | 混跑期间调用失败 → `GatewayPushClient` 返回 false → 被当成「离线」→ `ConfirmMessageDispatch` → **静默丢消息** | **升级必须整包替换，不可滚动发布**；若必须滚动，先在 PR-4 落地三态（`UNIMPLEMENTED` → `Unknown` → Kafka 不提交 offset） |

**总回滚开关**：

| 开关 | 作用 |
|---|---|
| `GATEWAY_ROUTING=0` | 关闭路由表读写，退回"单网关 + 广播"模式 |
| `GATEWAY_MULTI_INSTANCE=0` | 单实例注册（旧 etcd key 格式） |
| `--role=all` | 回到改造前的单进程全启动 |

---

---

## 9. 决策记录（已确认）

> 本章替代原先的「未决问题」。所有决策已确认，PR 实现以本表为准；后续若要变更，改这一张表并同步受影响的章节。

### 9.1 已确认决策

| # | 决策点 | 取值 | 影响 |
|---|---|---|---|
| **Q-1** | 网关实例 ID | 环境变量 `SERVICE_INSTANCE_ID` 注入，缺省 `hostname:port` | 重启后保持稳定，路由表旧值可复用；换端口即视为新实例 |
| **Q-2** | 路由表 TTL（`gw:user:{account}`） | **300s** | 实例挂掉最多滞留 5 分钟，由广播兜底吸收；心跳每 30s 续期，容错 10 次丢包 |
| **Q-3** | 广播兜底超时 | 单次 RPC **300ms**，广播整体截止 **500ms** | 超过即判 `Unknown`（→ Kafka 重试），低于 200ms 易误判 |
| **Q-4** | 旧 `PushToUser` 兼容接口 | **直接删除**，不做兼容层 | PR-2 一次性切换；服务端与客户端需同批上线，没有灰度窗口 |
| **Q-5** | 群 `Notice` 是否内联消息 | **保留**：小群（`member_count ≤ 200`）内联最近 20 条；大群只推 `lastSeq` | 与现网行为一致，客户端已实现 |
| **Q-6** | 客户端重连出口 | **需要**：连续失败 N 次后弹窗，用户选「重试 / 退出登录」 | N 建议 5 次（退避到 30s 后约 2 分钟） |
| **Q-7** | 补投起点安全窗口 | ~~200 条~~ → **新模型下不需要**（见 9.2） | 每会话水位精确到会话，不存在"全局窗口"问题 |
| **Q-8** | 全量对账频率 | ~~每 24h 首次登录~~ → **新模型下不需要**（见 9.2） | 客户端 cursor 全 0 时自然退化为"每会话推最近 20 条"，本身就是对账 |
| **Q-9A** | 登录同步粒度 | **登录时按会话各推最近 N 条** | 登录后即可看到每个会话的最新上下文，不必点开才拉 |
| **Q-9B** | 客户端水位粒度 | **扩展协议，上报每会话 `lastSeq`** | 精确知道每个会话缺到哪；需改 proto + 客户端 |
| **Q-9C** | 登录同步规模 | **每会话 N=20 条，最多 K=50 个会话** | 单次登录最多约 1000 帧；超出 K 的会话由客户端点开时主动拉 |
| **Q-10** | 下发节奏 | **写队列保持 256；按会话分轮下发，每轮 ≤50 帧** | 不一次性塞满队列；入队失败重试 5×50ms 后才断开 |

### 9.2 因模型变更而失效的两项

原 Q-7（安全窗口 200）和 Q-8（定期全量对账）是在"全局 `lastMsgSeq` 穷举补投"模型下的兜底手段。确认 Q-9B（每会话 `lastSeq`）后，两者都失去意义：

| 原决策 | 为什么不再需要 |
|---|---|
| 安全窗口 200 条 | 水位从"一个全局数字"变成"每个会话一个数字"，不再需要用窗口去覆盖跨会话的空洞 |
| 每 24h 首次登录全量对账 | 客户端 cursor 全 0（重装、清库、换设备）时，服务端本来就会对每个会话推最近 20 条——这本身就是一次对账，且开销远小于全量扫描 |

**保留的运维兜底**：提供一个人工触发的对账工具（`--reconcile-account=<account>`），只在怀疑数据不一致时使用，不进正常流程。

### 9.3 实现时仍需定、但不阻塞开工

| # | 待定项 | 建议 |
|---|---|---|
| S-1 | 客户端重连 N 次 | 5 次（1/2/4/8/16s 退避后约 31s，再失败即弹窗） |
| S-2 | 每会话 N 条是否按会话类型区分 | 不区分，统一 20 条；群聊有独立 `Pull` 通道，不依赖 200 帧 |
| S-3 | 每轮 50 帧的"轮"边界 | 以会话为单位切分，单会话超过 50 条时可能跨轮（本模型下 N=20，不会发生） |
| S-4 | 209 的 `hasMore` 语义 | 指"还有未同步的会话（超过 K）"，不是"还有未同步的消息" |

---

## 10. 离线消息同步与补投（按会话）

> 本章按 Q-9A/B/C 确认的模型重写，取代第一版的"穷举补投"。核心变化：**登录同步从「穷举所有未 ACK 消息」改为「按会话取有界快照」，历史由客户端主动拉取。**

### 10.1 模型

| 阶段 | 做什么 | 边界 |
|---|---|---|
| **登录同步** | 拉会话列表（`latest_time desc`），对每个会话推"客户端未收到的最新 20 条" | 最多 50 个会话 → 最多约 1000 帧 |
| **客户端主动拉** | 点开会话 / 向上滚动时，按 `afterSeq` 分页拉该会话历史 | 每页 20–100 条，客户端控制节奏 |
| **在线推送** | `type=104`，与登录同步并行 | 由路由表定位网关 |
| **不做** | 全量穷举补投、等 ACK 才推进、超时踢线、定期全量对账 | — |

**为什么这样更好**：

- 登录工作量从 **O(未 ACK 消息总数)** 降为 **O(K × N)**，与历史长度无关；
- 不再有"等 ACK"环节 → 彻底消除"60s 超时踢线 → 循环重连"（旧模型的 O-1/M-1）；
- 会话列表里的 `maxSeq` / `unreadCount` 天然告诉客户端"哪个会话还有东西没拉"，不需要额外机制。

### 10.2 现状缺陷（改造前）

| # | 缺陷 | 后果 |
|---|---|---|
| O-1 | 批内 60s 未 ACK → `onDisconnect()` | 一条客户端不回 ACK 的消息可让用户**永久登不进去**；多网关 + 自动重连后变成跨实例无限重连风暴 |
| O-2 | 脏数据 `throw` 中止整轮 | 一条 `servermsgid` 为空的消息让后面全部停发，且每次登录撞同一处 |
| O-3 | `after = 0` 全量扫描 | 登录耗时随历史线性增长 |
| O-4 | 每条候选一次 MySQL 事务 | 50 条一批 = 50 个事务、200+ 次查询 |
| O-5 | 每条 ACK 一次 gRPC | 50 条 = 50 次 RPC |
| O-6 | `handleAck` 失败裸 `close()` | 不注销 session、不写离线状态 → 僵尸连接 |
| O-7 | 补投与 104 并发，两条路径的 ACK 都清 `pendingOfflineMsgs_` | 批次"提前完成"，进度失真 |
| O-8 | 补投中途断线/下线 | 从 0 重来，无断点 |

新模型消除 O-1～O-5、O-8；O-6、O-7 单独修（见 10.7、10.8）。

### 10.3 协议扩展

**客户端上报每会话水位**（`proto/sdkws.proto`，追加字段，不改已有字段号）：

```proto
message ConversationCursor {
  string conversationID = 1;
  int64  lastSeq         = 2;  // 客户端该会话已持久化的最大连续 seq；0 表示没有
}

message SdkWSReq {
  // ... 1..7 保持不变
  repeated ConversationCursor cursors = 8;   // 替代 lastMsgSeq；首帧（type=101）携带
}
```

> `lastMsgSeq` 字段已删除；`cursors` 使用字段 8。

**同步结果摘要**（`type=209` 的 `data`，原来为空）：

```proto
message ConversationSyncRange {
  string conversationID = 1;
  int64  fromSeq         = 2;  // 本次下发的第一条 seq
  int64  toSeq           = 3;  // 本次下发的最后一条 seq
  bool   hasGap          = 4;  // 该会话在 fromSeq 之前仍有未同步消息
}

message SyncSummary {
  uint32 conversationsTotal    = 1;  // 会话总数
  uint32 conversationsSynced   = 2;  // 本次实际同步的会话数
  uint32 messagesPushed        = 3;  // 本次推送的 200 帧数
  bool   hasMore               = 4;  // 还有超出 K 的会话未同步
  repeated ConversationSyncRange ranges = 5;
}
```

**`209` 的 `errorCode` 语义**：

| errorCode | 含义 | 客户端行为 |
|---|---|---|
| `0` | 同步完成，无跳过 | 正常 |
| `PARTIAL` | 同步结束但有会话被跳过（脏数据）或有 `hasGap` | 提示"部分消息未同步"，**不要**提示重新登录 |
| `SYNC_ERROR` | 本轮彻底失败（下游不可用等） | 提示稍后重试 |

### 10.4 登录同步流程

```
认证成功
  ├─ 写路由（先写，让 104 能进来）
  ├─ co_spawn(doReadLoop)
  └─ syncOffline()
       cursors = req.cursors()  →  map<convID, lastSeq>
       convs   = MySQL.ListConversations(owner, is_hidden=0,
                                         order by latest_time desc, limit=50)

       pushed = 0; synced = 0; ranges = []
       for conv in convs:
           if disconnected_: break
           from = cursors[conv.id] ?: 0
           msgs = Mongo.LatestAfter(conv.id, from, 20)   // seq > from, 倒序取 20，再反转
           if msgs.empty(): continue

           for chunk in split(msgs, 50):                  // 本模型下 ≤20，恒为 1 块
               waitWriteQueueBelow(128)                   // 见 10.6
               for m in chunk: queuedWrite(200, m)        // 不等 ACK
               pushed += chunk.size()

           synced += 1
           ranges.push({conv.id, msgs.front.seq, msgs.back.seq,
                        hasGap: (msgs.front.seq > from + 1)})

       send 209 { conversationsTotal: convs.size(), synced, pushed,
                  hasMore: (convs.size() == 50),
                  ranges }
```

**关键点**：

1. **不需要等 ACK**。推出去就算完成；没落库的消息下次登录会因为 cursor 没前进而重新推。客户端按 `serverMsgID` 去重，天然幂等。
2. **`hasGap` 由服务端算**：`msgs.front.seq > from + 1` 说明客户端缺的超过 20 条，剩下的等客户端主动拉。
3. **`hasMore`** 表示"会话超过 50 个"，与消息无关。

### 10.5 客户端主动拉取

| 场景 | 触发 | 请求 |
|---|---|---|
| 点开会话 | 本地该会话最大 seq < 会话 `maxSeq` | `type=244`，`afterSeq=本地最大 seq`，`limit=20` |
| 向上滚动 | 用户滚到顶部 | 同上，持续分页直到无更多 |
| 收到 `hasGap` | 209 的 `ranges` 里 `hasGap=true` | 该会话首屏加载后自动补拉一页 |

`type=244`（`GetSingleChatHistory`）语义不变：`afterSeq` 之后升序、`limit` clamp(1,100)、返回 `hasMore` + `nextSeq`。

**水位更新**：客户端每持久化一条消息，若 `seq == 该会话 lastSeq + 1` 则推进 `lastSeq`；若出现空洞（`seq > lastSeq + 1`）则**不推进**，等补拉填洞。这样 `lastSeq` 始终表示"连续已收到"。

> 如果客户端不想维护连续性，也可以直接上报"本地最大 seq"，但那样空洞永远补不上（需要服务端再引入窗口）。**推荐维护连续水位**，成本只是一个 if。

### 10.6 下发节奏与写队列

| 参数 | 取值 | 说明 |
|---|---|---|
| 写队列容量 | 256（不变） | `startWriteQueue` |
| 每轮帧数 | ≤50 | 单会话 N=20，一轮一个会话，天然满足 |
| 轮间水位 | 队列剩余 < 128 时等待 | 避免和 104 抢占；等待有超时（1s） |
| 入队失败 | 重试 5 次 × 50ms | 仍失败才 `onDisconnect()`（客户端重连后按 cursor 续传） |

**为什么不再"一次性塞完"**：旧模型 50 条一批塞进 256 队列，遇到并发 104 就可能挤满 → 断线。新模型按会话分轮，每轮 20 帧，队列压力降到原来的 1/12。

### 10.7 ACK 处理（与旧模型的关键差异）

| 项 | 旧模型 | 新模型 |
|---|---|---|
| ACK 是否推进同步 | 是（等 ACK 才推下一批） | **否**（推完即完成） |
| ACK 是否阻塞后续帧 | 是（批内等待 60s） | 否 |
| ACK 失败处理 | 2 次重试后 `close()` | `co_await onDisconnect()`（幂等清理） |
| ACK 用途 | 同步推进 + 清理 | 清理 receipts/Redis 缓存 + 送达率统计 |

**`pendingOfflineMsgs_` 的语义收敛**：新模型下它只用于统计"这批 200 有多少被确认"，不再参与流程推进；`handleAck` 里按 `ackType` 区分，`ackType==1`（在线 104）不清补投统计。

### 10.8 与在线推送并存

- 路由表在同步开始前写好，所以同步期间 104 会到达；
- 客户端按 `serverMsgID` 去重（已有逻辑），按 `seq` 排序渲染（已有逻辑）；
- 网关**不保证** 200 与 104 的交叉顺序，这一点写进协议契约；
- 同一条消息既走 200 又走 104 时，两次 ACK 都接受（`im_message_receipts` 主键幂等）。

### 10.9 脏数据与异常

| 情况 | 处理 |
|---|---|
| `servermsgid` 为空 / 收件人不符 / seq 不递增 | 跳过该条，`sync_skipped_total{reason}` ++，`209.errorCode = PARTIAL` |
| 会话列表查询失败 | 发 `209 SYNC_ERROR`，连接保持（客户端可主动拉会话列表） |
| Mongo 查询失败 | 同上，跳过剩余会话 |
| 写队列持续失败 | 不发 209，`onDisconnect()`（客户端重连按 cursor 续传） |

**不再有 `throw` 中止整轮**——除了内部一致性错误（如 `size > batchSize`），那种保留抛出并告警。

### 10.10 指标

| 指标 | 类型 | 用途 |
|---|---|---|
| `sync_duration_seconds` | histogram | 登录同步耗时 |
| `sync_conversations_total` | counter | 本次扫描的会话数 |
| `sync_messages_pushed_total` | counter | 实际下发的 200 帧数 |
| `sync_skipped_total{reason}` | counter | 脏数据跳过 |
| `sync_gap_conversations` | gauge | `hasGap=true` 的会话数（反映客户端积压） |
| `write_queue_wait_seconds` | histogram | 轮间等待队列的时间 |
| `ack_timeout_total` | counter | ACK 未在窗口内到达（仅统计，不动作） |

### 10.11 PR-8（P1）按会话同步替代穷举补投

| 项 | 内容 |
|---|---|
| 目标 | 登录同步改为有界快照；消除 O-1～O-5、O-8；`209` 带摘要 |
| 改动 | `ws_session.cpp`（`syncOffline` 重写、删 `waitOfflineBatch` 的踢线分支）；`proto/sdkws.proto`（`ConversationCursor`、`SyncSummary`）；`mongohandler`（`LatestAfter(convID, afterSeq, limit)`，走已有 `{convID, seq}` 索引）；`mysqlhandler`（复用 `GetAllConversations`，补 `latest_time desc limit K`）；`push.proto` + `push_service`（可选：批量 ACK）；客户端 `chatService.cpp`（cursor 上报、gap 补拉、连续水位维护） |
| 前置 | PR-2（推送接口泛化）；建议在 PR-3/4 之后做（路由先写） |
| 风险 | 中（改登录主路径）。开关 `SYNC_V2=0/1` 灰度 |
| 回滚 | 关开关回到旧逻辑（旧代码保留一个版本） |

**验收命令**：

```bash
# 0) 开关关闭时行为不变（回滚验证）
SYNC_V2=0 ./main --role=gateway &

# 1) 50 会话 × 每会话积压 100 条 -> 每会话只推 20 条
#    指标（Grafana/OTel）：sync_messages_pushed_total 增量 ≤ 50×20 = 1000
#                          sync_gap_conversations == 50
#    客户端：209 解析出 50 项 ranges，每项 hasGap == true

# 2) 客户端不回 ACK -> 209 照常发出、连接不断
#    对照旧实现（60s 后 onDisconnect）；新实现应无断线

# 3) 客户端 cursor 全 0（清库/重装）-> 每会话推最近 20 条，不是全量扫
#    对照实验：同一账号历史 1 万条 vs 10 万条，sync_duration_seconds 差异应 < 20%

# 4) 单会话积压 5 条 -> 只推 5 条，hasGap == false，客户端本地 seq 连续

# 5) 脏数据 -> 跳过并 PARTIAL
#    人为造一条 servermsgid 为空的 Mongo 文档：
#    209.errorCode == "PARTIAL"，sync_skipped_total{reason="no_id"} 增 1，其余会话不受影响

# 6) 同步期间收到 104 -> 去重
#    客户端该 serverMsgID 只落库一条；两次 ACK 均幂等（im_message_receipts 主键）

# 7) 写队列不被同步打满
#    write_queue_wait_seconds 在预期范围；无 "queue full" 引起的 onDisconnect
```

### 10.12 验收矩阵补充

| # | 场景 | 期望 | 依赖 |
|---|---|---|---|
| V-12 | 50 会话 × 积压 100 条 | 每会话推 20 条，`hasGap=true`，`messagesPushed ≤ 1000` | PR-8 |
| V-13 | 客户端不回 ACK | 209 正常发出，连接不断（与旧实现的关键差异） | PR-8 |
| V-14 | 客户端清库（cursor 全 0） | 每会话推最近 20 条，耗时与会话数线性、与历史长度无关 | PR-8 |
| V-15 | 同步中断线 / 网关下线 | 重连后按 cursor 续传，不重复推已有消息 | PR-8、PR-3 |
| V-16 | 同步期间的 104 | 与 200 交叉到达，客户端按 `serverMsgID` 去重、按 `seq` 排序 | PR-8、PR-4 |
| V-17 | 脏数据 | 跳过并 `PARTIAL`，其余会话不受影响 | PR-8 |
| V-18 | 会话数 > 50 | 209 `hasMore=true`；未同步会话由客户端点开时拉取 | PR-8 |
| V-19 | 客户端只收到部分（有空洞） | cursor 停在空洞前，下次登录仍会推该段，不跳过任何消息 | PR-8、客户端 |
| V-20 | 同步期间写队列压力 | 每轮 ≤50 帧、队列剩余 <128 时等待；不出现因队列满而断线 | PR-8 |
| V-21 | `SYNC_V2=0` 回滚 | 回到旧行为（全量扫 + 等 ACK），功能仍可用 | PR-8 |