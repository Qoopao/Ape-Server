# Gateway / Auth 历史审计（2026-11）

> 状态：历史（已废弃）  
> 权威性：本文是「2026-11 网关/认证审计」的历史记录，已被当前代码取代  
> 最后核对：2026-11-26 @ `03ed7c2` + 工作区改造  
> 适用代码：src/gateway/、src/services/auth_service/

> 行号与部分结论早于多网关改造，**不得作为现状**；仅用于追溯。

> 分析范围：`src/gateway/*`、`include/gateway/*`、`src/services/auth_service/*`、`include/services/auth_service/*`、`src/services/gateway_push_service/server.cpp`、`src/main.cpp`、`proto/sdkws.proto`、`proto/apeauth.proto`、`proto/gateway_push.proto`、`CMakeLists.txt`，以及被引用的 `util/account_pool.*`、`util/snowflake.h`、`services/base_service.h`、`util/ioc_pool.h`、`data` 层调用点。
> 所有结论均给出 `文件:行号`。无法从代码确认的内容标注为「未实现/未确定」。
>
> **更正（2026-11-21）**：本报告关于「`queuedWrite` 是唯一真实抛出源、由 `start()` 的 catch 兜住」的表述**不准确**。复核结论：`queuedWrite` 唯一的抛出点在 `handleAuth` 认证成功帧处（`ws_session.cpp` 认证成功段），此时写队列刚创建、必然为空，`try_send` 必定成功，因此该 `throw` 实际**不可达**；`doReadLoop` 内的 `queuedWrite` 调用由 `co_spawn` 自带的 catch 收口；`start()` 的 catch 在正常流程中兜不到任何异常，它真正的作用是「保证 `onDisconnect()` 一定执行」（协程里没有 `finally`）。详见《服务端需求文档》§4.1 与对话记录。

---

## 1. 进程与启动

### 1.1 单进程多服务模型

`main.cpp` 只启动**一个进程**，进程内并行运行 8 个 gRPC 服务 + 1 个 WebSocket 网关 + 1 组 Kafka 生产/消费线程。全部服务由 `BaseServiceServer<ServiceType>` 模板托管（`include/services/base_service.h`）：`Start()` 内部 `std::thread` 跑 `RunServer()`（`base_service.h:58-76`），即**每个 gRPC 服务一个独立线程**。

| 顺序 | 服务 | 监听地址 | 注册到 etcd 的方法名 | 代码位置 |
|---|---|---|---|---|
| 0 | `BackbonService` | `0.0.0.0:50052` | 不注册自己（它是注册通道） | `main.cpp:62-64` |
| 1 | `AuthService` | `0.0.0.0:50051` | `Register`, `Login`, `ValidateToken` | `main.cpp:85-89` |
| 2 | `MsgService` | `0.0.0.0:50053` | 24 个方法（`GetMaxSeq` … `GetLastMessage`） | `main.cpp:95-122` |
| 3 | `GroupService` | `0.0.0.0:50056` | `Execute`（另起 `StartDelivery()` 投递协程） | `main.cpp:126-130` |
| 4 | `FriendService` | `0.0.0.0:50057` | `Execute` | `main.cpp:132-134` |
| 5 | `ConversationService` | `0.0.0.0:50058` | `GetAllConversations` 等 6 个 | `main.cpp:136-141` |
| 6 | `GatewayPushService` | `0.0.0.0:50055` | `PushToUser` | `main.cpp:146-150` |
| 7 | `PushService` | `0.0.0.0:50054` | `PushMsg`, `DelUserPushToken` | `main.cpp:164-175` |
| 8 | `WebServer`（WebSocket 网关） | `0.0.0.0:6666` | 不注册（纯接入层） | `main.cpp:202-209` |

### 1.2 启动顺序（严格按代码）

1. `ape::otel::InitTracer/InitMeter/InitLogger("ape-server")`（`main.cpp:33-35`）。
2. `Snowflake::init(...)`，读环境变量 `SNOWFLAKE_WORKER_ID`、`SNOWFLAKE_DATACENTER_ID`，缺省均为 `1`（`main.cpp:38-43`）。这是**全仓唯一读取环境变量的地方**（`grep getenv` 仅 4 处命中，全在 `main.cpp`）。
3. `RedisConnector::instance().start(4)` → 硬编码 `127.0.0.1:6379`（`redisconnector.cpp:47-48`）。
4. `MySQLConnector::instance().start(2)` → 硬编码 `127.0.0.1:3306`（`mysqlconnector.cpp:43`）；随后用 `co_spawn(...).get()` **同步阻塞**执行 `InitializeConversations()`、`InitializeOutbox()`、`InitializeGroups()`（`main.cpp:50-55`）。
5. `MongoConnector::instance().start(4)`（`main.cpp:58`）。
6. `BackbonService` 启动，并**指数退避等待**其进入 `ServiceState::kRunning`（1s、2s、4s）；第 3 次仍失败则清理 OTel 并 `return 0`（`main.cpp:67-80`）。
7. 依次启动 Auth/Msg/Group/Friend/Conversation/GatewayPush（`main.cpp:83-150`）。
8. 创建 `KafkaProducer("msg_topic")` 并注入 MsgService（`main.cpp:94, 154`）。
9. 创建独立 `IOC_Pool kafka_ioc_pool(4)`（`main.cpp:158-160`），`KafkaConsumer("push-service-group", {"msg_topic"})` 注入 PushService（`main.cpp:170-173`）。
10. `push_server->Start()`、`msg_server->StartOutbox()`（`main.cpp:175-176`）。
11. 对 auth/msg/gateway_push/push/group/friend/conversation 逐个 `WaitForEtcdRegistration(30)`（每 500ms 轮询一次，`base_service.h:135-152`）；超时**仅告警，继续启动**（`main.cpp:180-198`）。
12. `WebServer server(4, 6666, backbon_client)` → `server.start()`（`main.cpp:208-209`）。`start()` 内部 `get_acceptor_ioc().run()` 阻塞，**main 线程在此处挂住直到 SIGINT/SIGTERM**（`webserver.cpp:88-99`）。
13. 返回后 `StopOutbox()`、`StopDelivery()`、OTel 清理；其余服务的停止由 `unique_ptr` 析构触发 `BaseServiceServer::~BaseServiceServer → Stop()`（内部会 `UnregisterFromEtcd()`，`base_service.h:49-55, 94`）。

### 1.3 etcd 注册的服务名与地址格式

- key：`"/services/" + service_name`，即 `/services/AuthService`、`/services/GatewayPushService` 等（`backbon_service/server.cpp:131`、`234`、`286`）。
- value：`serialize_vector(ip_ports)`，**逗号分隔**的 `ip:port` 列表；实际只 push 了一个 `listen_address_`，即 `"0.0.0.0:50051"` 这种形式（`base_service.h:165-168`、`backbon_service/server.cpp:23-32`）。
- 租约：`etcd_client.leasegrant(get_lease_ttl())`，`DEFAULT_LEASE_TTL = 300000`，注释写 `//30`（`etcdconnector.h:49`），然后 `put(key, value, lease_id)` 并启动 `KeepAlive` 自动续约（`backbon_service/server.cpp:139-188`）。
- 镜像写入 Redis：`setex(service_name, 60, serialized_methods)`，TTL 60s（`backbon_service/server.cpp:197-207`）。
- etcd 端点：`http://127.0.0.1:2379`（`etcdconnector.h:50`）。

**注意**：网关 `discover_Service` 取 `resp.ipport(0)` 直接 `grpc::CreateChannel(addr, InsecureChannelCredentials())`（`webserver.cpp:31-33, 67-80`）。注册值是 `0.0.0.0:PORT` 而非可路由地址，Linux 下能连上（等价回环），但语义不规范。

### 1.4 配置项清单（全部硬编码，无配置文件）

| 项 | 值 | 位置 |
|---|---|---|
| Redis | `127.0.0.1:6379` | `redisconnector.cpp:47-48` |
| MySQL | `127.0.0.1:3306` | `mysqlconnector.cpp:43` |
| etcd | `http://127.0.0.1:2379` | `etcdconnector.h:50` |
| TLS 证书 | `/root/ape-server/scripts/certs/server.crt` | `webserver.cpp:54` |
| TLS 私钥 | `/root/ape-server/scripts/certs/server.key`（PEM） | `webserver.cpp:55` |
| 网关端口 | `6666`，IOC 池 4 个 worker | `main.cpp:208` |
| Snowflake | 环境变量 `SNOWFLAKE_WORKER_ID` / `SNOWFLAKE_DATACENTER_ID` | `main.cpp:38-43` |

`ssl_ctx_.use_certificate_chain_file/use_private_key_file` 在 `WebServer` **构造函数**中同步执行（`webserver.cpp:54-55`）；文件缺失会抛 `boost::system::system_error`，被 `main.cpp:220` 的 `catch` 吞掉并 `return 0` —— 即**启动失败也返回退出码 0**。

### 1.5 构建

`CMakeLists.txt`：C++20 + vcpkg（triplet `x64-linux`），`file(GLOB_RECURSE src/*.cpp)` 剔除 `main.cpp` 打成一个 `ape-server` 静态库，`main` 可执行文件只链接该库（`CMakeLists.txt:26-37, 93-94`）。**没有**把 gateway 单独编译成库；`Boost::beast`、`gRPC`、`OpenSSL::Crypto` 都链在同一个库里。测试目标全部由 `option()` 关闭（`APE_REGISTRATION_TESTS`、`APE_GROUP_TESTS`、`APE_MONGO_INDEX_TESTS`、`APE_OFFLINE_LOGIN_TESTS`）。

---

## 2. WebSocket 接入

### 2.1 握手流程

1. `WebServer` 构造：`ssl::context{tls_server}` + 加载证书与私钥（`webserver.cpp:45-56`）。
2. `listener()` 在 `_acceptor_ioc` 上 `async_accept(executor, use_awaitable)`，`executor = make_strand(_iocPool.get_ioc(nextWorker))`，round-robin 取 worker（`webserver.cpp:121-128`）。
3. 用裸 socket 构造 `ssl::stream<tcp::socket>`（共享 `ssl_ctx_`），`co_spawn` 到同一 strand 上运行 `handle_ws_session`（`webserver.cpp:130-138`）。
4. `WSSession::start()` **双层 co_spawn**：外层协程内先 `ws_.next_layer().async_handshake(ssl::stream_base::server)` 做 TLS 握手，再 `ws_.async_accept()` 做 WebSocket 握手（`ws_session.cpp:52-68`）。任一步失败仅记日志 `co_return`（无 close 帧）。
5. 握手成功后 `await doRead()` 读第一帧并分发（`ws_session.cpp:79`）。`doRead()` 只在抛异常时才会调 `onDisconnect()`（`ws_session.cpp:84-85`）。

**路径**：代码中**没有任何 URL 路径/Origin/子协议校验**，Beast 的 `async_accept` 不检查 request target，任何路径均可接入。「路径」未实现。

### 2.2 帧格式

- 所有出站帧前都调 `ws_.binary(true)`，统一发**二进制帧**（如 `ws_session.cpp:111, 172, 312, 487, 589`）。
- 入站帧无 opcode 检查，直接 `beast::buffers_to_string` 后 `sdkws::SdkWSReq::ParseFromString`（`ws_session.cpp:399-404, 461-471`）。解析失败在首帧阶段回 `errorCode="1"` 并 `close(policy_error)`；在消息循环阶段仅 `spdlog::debug` 后 `continue`（`ws_session.cpp:466-471`）。
- 请求封装 `sdkws.SdkWSReq`：`requestId(1) token(2) account(3) deviceID(4) data(5,bytes) type(6) trackID(7) lastMsgSeq(8)`（`sdkws.proto:340-351`）。
- 响应封装 `sdkws.SdkWSResp`：`requestId(1) token(2) account(3) deviceID(4) errorCode(5) errorMsg(6) data(7,bytes) type(8) trackID(9) logID(10)`（`sdkws.proto:353-365`）。
- 实际使用情况：`token` 只在认证成功响应中回填（`ws_session.cpp:308`）；`deviceID` 只做回显，**不参与任何索引**（`ws_session.cpp:160, 309`）；`trackID` 只在日志里打印（`ws_session.cpp:206`）；`lastMsgSeq` **全仓 0 处读取**（离线拉取恒从 seq 0 开始，见 §4.4）；`logID` **从未赋值**。

### 2.3 完整 type 码表（以 `ws_session.cpp` 分支为准）

#### A. 认证前（`doRead()`，`ws_session.cpp:424-434` 的 `switch`）

| type | 语义 | 请求 `data` 载荷 | 响应 `type` | 响应 `data` 载荷 | 位置 |
|---|---|---|---|---|---|
| `100` | 注册账号 | `auth.RegisterRequest{phone,nickname,password}` | `0` | 无（仅 `account`） | `ws_session.cpp:425-427` → `handleReg` `91-176` |
| `101` | 登录/认证 | `token` **为空**时 `data` = 明文密码字符串（**非 protobuf**）；`token` 非空时 `data` 被忽略 | `101`（回显 `req.type()`） | 无（回填 `token`、`deviceID`） | `ws_session.cpp:428-430` → `handleAuth` `178-352` |
| 其它 | 不支持 | — | `0` | 无，`errorCode="4401"`，随后 `close(close_code(4404))` | `ws_session.cpp:431-433` → `fallback` `354-380` |

#### B. 认证后（`doReadLoop()`，`ws_session.cpp:445-709` 的 `if` 链）

| type | 语义 | 请求 `data` 载荷 | 响应 `type` | 响应 `data` 载荷 | 位置 |
|---|---|---|---|---|---|
| `0` | **心跳 ping** | 无（`data` 忽略） | `0` | 无；`errorCode="0"`、`errorMsg="pong"` | `ws_session.cpp:478-499` |
| `101` | **发单聊消息** | `sdkws.SendMessageReq{repeated MsgData msgs}` | `101` | `sdkws.SendMessageResp` | `ws_session.cpp:502-566` |
| `106` | **统一消息 ACK**（在线/离线通用） | `sdkws.AckReq{userID, repeated serverMsgIDs, ackType∈{1,2}}` | 无响应帧 | — | `ws_session.cpp:569-572` → `handleAck` `869-904` |
| `250` | 群操作/群消息命令 | `groupchat.Command` | `250` | `groupchat.Reply` | `ws_session.cpp:575-592` |
| `244` | 拉取单聊历史消息 | `msg.GetSingleChatHistoryReq{ownerUserID,peerUserID,…}` | `244` | `msg.GetSingleChatHistoryResp` | `ws_session.cpp:595-617` |
| `240` | 获取全部会话 | 无（`data` 忽略） | `240` | `conversation.GetAllConversationsResp` | `ws_session.cpp:629-631` |
| `241` | 会话标记已读 | `conversation.MarkConversationReadReq` | `241` | **无 data**（`MarkRead` 返回 `void`） | `ws_session.cpp:632-637` |
| `242` | 更新会话设置 | `conversation.UpdateConversationSettingsReq` | `242` | **无 data**（返回 `void`） | `ws_session.cpp:638-643` |
| `243` | 删除会话 | `conversation.DeleteConversationReq` | `243` | **无 data**（返回 `void`） | `ws_session.cpp:644-650` |
| `220` | 好友：LookupUser | `friendpb.Command` | `220` | `friendpb.Reply` | `ws_session.cpp:660-703`；`operation = reqType-220`（`671`） |
| `221` | 好友：Apply | 同上 | `221` | 同上 | 同上 |
| `222` | 好友：ListFriends | 同上 | `222` | 同上 | 同上 |
| `223` | 好友：ListApplications | 同上 | `223` | 同上 | 同上 |
| `224` | 好友：HandleApplication | 同上 | `224` | 同上 | 同上 |
| `225` | 好友：DeleteFriend | 同上 | `225` | 同上 | 同上，`operation=5` |
| 其它 | **未实现**：只打 `debug` 日志，**不回任何帧** | — | — | — | `ws_session.cpp:705-708` |

补充说明（重要，易误读）：

- **`101` 是双重语义**：认证前是"登录"，认证后是"发消息"。二者在同一连接上不冲突（阶段不同），但**响应帧的 `type` 都是 `101`**，客户端只能靠 `data` 是否为空 / `errorMsg` 区分（`ws_session.cpp:306` 与 `532`）。
- **`250` 不把 `Reply.errorCode` 提升到 `SdkWSResp.errorCode`**：只写 `type/account/requestId/data`（`ws_session.cpp:586-590`），错误码只能从 `data` 里的 `groupchat.Reply` 解析。对比 `220-225` 是提升的（`ws_session.cpp:684-685`）。
- **`244` 的默认 `errorCode="0"` 先写入再被覆盖**（`ws_session.cpp:600-613`）；`INVALID_PARAM` 同时用于"data 解析失败"和"peerUserID 为 0"。
- **`241/242/243` 成功时不写 `data`**，但 `errorCode` 已预置 `"0"`（`ws_session.cpp:625`）。
- `220-225` 在 `reply.errorCode()=="0"` 时，还会向其 `changedAccounts()` 中**在线**用户额外推 `type=230` 通知帧（见下表）；不在线者跳过并 `debug` 日志 `"Friend notification deferred to login sync"`（`ws_session.cpp:688-701`）。

#### C. 服务端主动下推帧（非 `ws_session.cpp` 分支，但同一 `SdkWSResp` 封装）

| type | 语义 | `data` 载荷 | `errorCode`/`errorMsg` | 产生位置 |
|---|---|---|---|---|
| `104` | 在线消息下推 | `sdkws.MsgData`（透传 `PushToUserReq.msgDataBin`） | 未设置 | `gateway_push_service/server.cpp:33-38` |
| `200` | 离线消息重放（单条） | `sdkws.MsgData` | `"0"` / `"offline message"` | `ws_session.cpp:841-848` |
| `209` | 离线重放**完成**通知 | 无 | `"0"` / `"offline sync complete"` | `ws_session.cpp:798-801` |
| `209` | 离线重放**失败**通知 | 无 | `"SYNC_ERROR"` / `"Offline message synchronization failed; sign in again to retry"` | `ws_session.cpp:343-349` |
| `230` | 好友关系变更通知 | 无 | `"0"` | `ws_session.cpp:694-699` |
| `251` | 群变更刷新通知 | `groupchat.Notice` | 未设置 | `group_service/server.cpp:46-50, 76-77` |

- `200` 帧的 `requestId` 被填成 `message.serverMsgID()`（`ws_session.cpp:843`），**不是**原始请求 ID。
- `104` 下推帧的 `requestId`、`errorCode`、`deviceID` **全为空**（`gateway_push_service/server.cpp:33-36`）。

#### D. proto 注释里声明但**未实现**的 type

`sdkws.proto:349-350` 注释列出 `102：拉取单链`、`103：拉取混链`、`105：下推命令消息`：

- `102`/`103`：网关**无任何处理分支**。首帧阶段收到会走 `fallback`（`errorCode="4401"` + `close(4404)`）；认证后收到只落 `debug` 日志、无响应（`ws_session.cpp:705-708`）。
- `105`：全仓无任何 `set_type(105)` 的产生者（`grep set_type(` 共 24 处命中，无 105）。**未实现**。
- `sdkws.CmdMessage` / `MessageUnion` 结构体已定义（`sdkws.proto:279-289`），但网关不组装也不解析。

### 2.4 关闭语义

| 场景 | close code | 位置 |
|---|---|---|
| 首帧 Protobuf 解析失败 | `websocket::close_code::policy_error`（1008） | `ws_session.cpp:419` |
| 注册 `data` 解析失败 / 字段缺失 | `policy_error`（1008） | `ws_session.cpp:114, 135` |
| 认证缺 `account` / token 无效 / 密码错 | `policy_error`（1008） | `ws_session.cpp:195, 229, 277` |
| 不支持的首帧 type | `close_code(4404)`（**自定义码**） | `ws_session.cpp:372` |
| 正常断线 / `onDisconnect()` | **不发送 WS close 帧**，直接 `socket.cancel → shutdown(both) → close` | `ws_session.cpp:913-917`（注释在 `933`） |
| `handleAck` 重试仍失败 | 直接 `beast::get_lowest_layer(ws_).close(ec)`，不发 close 帧 | `ws_session.cpp:892-896` |

---

## 3. 认证

### 3.1 注册流程

1. 客户端发 `SdkWSReq{type=100, requestId, data=auth.RegisterRequest}`。
2. 网关 `handleReg` 校验 `phone/nickname/password` 非空（`ws_session.cpp:119-138`），缺字段回 `errorCode="IMCOMPLETE_INFO"` 后关连接。
3. `AuthClient::RegisterRequest(phone, nickname, password, requestId)`：`requestId` 通过 gRPC metadata `x-registration-id` 传递，**deadline 15s**（`auth_service/client.cpp:52-66`）。
4. 服务端 `AuthServiceImpl::Register` 校验：`requestId` 非空、≤64、仅 `[A-Za-z0-9-]`；`phone` 必须 11 位且 `[0]=='1'`、`[1]∈'3'..'9'`；`nickname` 非空 ≤256；`password` 非空 ≤1024。不合法回 `errorCode="INVALID_INFO"`（`auth_service/server.cpp:80-101`）。
5. 盐 + 哈希 + 落库在 `MySQLConnector` **pool 0 executor** 上执行（`auth_service/server.cpp:103-119`）：`password_salt = generate_salt()`，`password_hash = hash_password(password, salt)`，随后 `MySQLHandler::register_user(...)` 在一个事务里完成账号分配与用户创建。
6. 响应 `AuthRegResponse{success, user{userID, account, phone, nickname}}`。
7. 网关回 `SdkWSResp{type=0, account=新账号}`，**不签发 token、不进入认证态**（`ws_session.cpp:157-175`）。

注册的业务错误码来自 `MySQLHandler::RegistrationError`（`mysqlhandler.h:76-81`，`mysqlhandler.cpp:308` 有 `using Error = RegistrationError`）：`PHONE_EXISTS`（`mysqlhandler.cpp:463, 512`）、`ACCOUNT_RANGE_EXHAUSTED`（`570`）、`ACCOUNT_POOL_UNAVAILABLE`（`612`），在 `auth_service/server.cpp:130-132` 被原样回填 `errorCode`。

账号池：`AccountPoolAllocator::kMinAccount = 1000000000ULL`，`kMaxAccount = 9999999999ULL`，`kLowWaterMark = 1000`，`kRefillBatch = 10000`（`account_pool.h:10-13`）；`generate_shuffled_batch` 生成区间后 `std::shuffle`（线程局部 `mt19937_64`，`account_pool.cpp:16-18`）。`AccountPoolTool::refill/ensure_pool_not_low` 只是 `MySQLHandler::refill_accounts` 的薄封装，**必须在 pool 0 executor 上调用**（`account_pool.cpp:21-27`）。

### 3.2 登录流程

`handleAuth`（`ws_session.cpp:178-352`）两条分支：

- **分支 1（`token` 为空 → 密码登录）**：`AuthClient::LoginRequest(account, data)` → `AuthServiceImpl::Login`。服务端校验 `account!=0 && password` 非空（否则 `INVALID_PARAM`），查 `find_user_by_account`，`hash_password(password, user.password_salt)` 与库里 `password_hash` **字符串等值比较**（非恒定时间比较），失败 `WRONG_PASSWORD`；成功后 `update_last_login`，写 Redis token，回 `AuthRegResponse{success, token, expiresAt, user{...}}`（`auth_service/server.cpp:152-244`）。
- **分支 2（`token` 非空 → token 校验）**：`AuthClient::ValidateToken(token)`，要求 `resp.valid() && resp.account() == req.account()` 同时成立（`ws_session.cpp:244-249`）。任一不满足 → `token_valid=false`。

### 3.3 token 生成、存储、TTL

- **不是 JWT**。`generate_token()` = `boost::uuids::to_string(random_generator()())`，即标准 36 字符 UUID 字符串（`auth_service/server.cpp:61-63`）。
- 存储：Redis `SETEX`，key = `"token:" + token`（前缀 `TOKEN_KEY_PREFIX = "token:"`，`auth_service/server.cpp:28`），value = `"{user_id},{account}"`（`auth_service/server.cpp:212-214`）。
- TTL：`TOKEN_TTL_SECONDS = 7 * 24 * 3600`（7 天，`auth_service/server.cpp:31`）。
- `expiresAt`：登录时算 `now + 7d` 的 Unix 秒（`auth_service/server.cpp:65-71`），**与 Redis 实际 TTL 无强一致保证**。
- **无续期**：`ValidateToken` 只 `get` + `ttl`，**不刷新 TTL**（`auth_service/server.cpp:264-312`）。因此 token 硬过期 7 天。
- **无吊销/登出接口**：`apeauth.proto:5-9` 只有 `Register/Login/ValidateToken` 三个 rpc，无 `Logout/Revoke`。

### 3.4 ValidateToken 语义

输入 `token` → 空则 `valid=false`（`auth_service/server.cpp:257-262`）→ `GET token:{token}`；缺失即 `valid=false`（过期/不存在不可区分）→ 值必须含 `,`，否则 `valid=false` 并 `error` 日志 `malformed value`（`286-294`）→ `stoull` 拆出 `user_id`、`account` → 查 `ttl`，若 `ttl>=0` 则 `expiresAt = now + ttl` → 回 `{valid=true, userID, account, expiresAt}`。gRPC 侧异常回 `INTERNAL "Redis error"`；`AuthClient` 见 `!status.ok()` 时**吞掉错误**返回 `valid=false`（`auth_service/client.cpp:19-23`），即 Redis 故障与 token 失效对网关不可区分。

### 3.5 密码存储

- 算法：`SHA-256(salt || password)`，`EVP_DigestInit_ex(EVP_sha256())`，小写 hex 输出 64 字符（`auth_service/server.cpp:37-54`）。**单轮 SHA-256，无 PBKDF2/bcrypt/argon2，无迭代次数**。
- 盐：`generate_salt()` = UUID 字符串的**前 32 个字符**（`auth_service/server.cpp:56-59`）。注意 `to_string` 带连字符，所以这 32 字符形如 `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxx`，**包含 4 个 `-`**，实际随机熵低于 128bit。
- 落库字段：`user.password_salt`、`user.password_hash`（`auth_service/server.cpp:110-111`）。

### 3.6 认证成功后的动作（顺序）

`account_ = account`（`ws_session.cpp:283`）→ `WSSessionManager::registerSession`（`287`）→ `startWriteQueue()`（`290`）→ Redis `setex("user:{account}:online", 300, "1")`（`293-295`）→ 回认证成功帧（`302-313`）→ `co_spawn(doReadLoop)`（`316-330`）→ `pullAndPushOfflineMsgs()`（`335`）。注意：**写队列与注册发生在活动连接建立之前**，此时若 `queuedWrite` 失败会直接 `onDisconnect`（`775-780`）。

---

## 4. 连接生命周期

### 4.1 连接注册表

`WSSessionManager` 是**进程内单例**（`static WSSessionManager mgr`，`ws_session_manager.cpp:7-10`），结构为：

```
std::shared_mutex mutex_;
std::unordered_map<uint64_t /*account*/, std::shared_ptr<WSSession>> sessions_;
```

- `registerSession`（`19-31`）：`unique_lock`，若 key 已存在则**直接覆盖**（`isReplacing=true`，`WsConnectionsActive` 不加 1）；否则 `WsConnectionsActive().Add(1)`。
- `unregisterSession(account, session)`（`34-48`）：仅当 `it->second == session`（`shared_ptr` 身份比较）才删除并 `Add(-1)`，否则日志 `"stale session for user={}, skipping"` 返回 `false`。这一句是"旧连接不清在线状态"的关键（见 §7）。
- `pushToUser`（`52-66`）：`shared_lock` 查表，**`unlock` 之后再** `session->asyncSend(payload)`（注释说避免死锁），找不到返回 `false`。
- `hasSession` / `onlineAccounts`（`12-17, 68-71`）：读锁遍历。

键是 **account（10 位账号）**，不是 MySQL 的 `user_id`；`deviceID` 不参与索引，因此**同账号多设备互斥为单连接**。

### 4.2 心跳与超时

- 客户端 ping：`SdkWSReq.type == 0`（`ws_session.cpp:478`）。服务端回 `type=0, errorCode="0", errorMsg="pong"` 并**续期** Redis 在线 key 到 300s（`ws_session.cpp:479-498`）。
- 服务端**没有主动心跳定时器**：全靠客户端 ping 触发续期。若客户端不 ping，300s 后 key 过期，但连接仍存活（`doReadLoop` 不检查在线 key）。
- WS 层超时：`doRead()` 与 `doReadLoop()` 都设置 `websocket::stream_base::timeout::suggested(beast::role_type::server)`（`ws_session.cpp:387-388, 442-443`）——具体 idle 秒数由 Boost 版本决定，代码未显式取值，**未确定**。

### 4.3 在线状态

- key：`"user:{account}:online"`，值 `"1"`（在线）/`"0"`（离线），TTL 恒为 `300` 秒。
- 置 1：认证成功（`ws_session.cpp:294-295`）、每次心跳（`492-494`）。
- 置 0：`onDisconnect` 且**本 session 是活跃 session** 时（`ws_session.cpp:920-931`）。
- 消费方：`PushService` 用 `"user:"+recvID+":online"` 判断在线（`push_service/server.cpp:123-125`）；`BackbonService::CheckUserOnline` 用它并回填 `connectionCount=1`（`backbon_service/server.cpp:75-79`）。
- 注意 `onDisconnect` 的置 0 也是 `setex(..., 300, "0")`，**不是** `del`，因此离线态最多滞留 5 分钟（`docs/gateway_issues.md` 的 P5 描述已与当前代码不符）。

### 4.4 离线消息拉取与推送（批处理细节）

调用链：`handleAuth` → `pullAndPushOfflineMsgs`（`ws_session.cpp:784-803`）→ `scanOfflineMessages`（`805-832`）。

1. **单一数据源**：`MongoHandler::GetMsgsBySeqFromMongoAsync(account_, after, limit)`（`mongohandler.h:58`）。Redis 离线 ZSET **不参与**分页拼接（注释明确，`ws_session.cpp:786-787`），避免"最大序号跨过缓存缺口"。
2. **批次大小**：`constexpr int batchSize = 50`（`ws_session.cpp:808`）。返回条数 > 50 直接 `throw std::runtime_error("Offline batch exceeds limit")`（`816`）。
3. **游标**：`after` 从 **0** 开始，每批结束推进到本批最后一条的 `seq`（`811, 830`）。这是"本轮扫描游标"，不是持久 ACK 水位（注释 `809-810`）。
4. **逐条校验**：`seq` 必须严格递增（`message.seq() <= next` 即抛 `"Invalid offline candidate or sequence order"`）；必须是本人单聊消息 —— `message_validation::IsSingleMessageFor` 要求 `owner && !isGroupMsg && sessionType==1 && recvID==owner && sendID && !serverMsgID.empty()`（`message_validation.h:8-11`）。
5. **逐条下发**：`deliverOfflineMessage` → `MySQLHandler::WithFriendMessage(message, action, requireUndelivered=true)`（`ws_session.cpp:837-852`）。在账号对锁内再次检查好友关系、删除水位、正文提交阶段与 MySQL 持久化回执，**已 ACK 的消息不重复下发**。
6. **先登记后入队**：`pendingOfflineMsgs_[serverMsgID] = {seq, convID}` 在 `queuedWrite` **之前**写入（`ws_session.cpp:847-848`），注释说明是为了防止"快速 ACK 到达时找不到待确认项"。
7. **批次 ACK 等待**：每批下发完 `co_await waitOfflineBatch()`（`828`），默认超时 **60 秒**（`ws_session.h:79`）。实现是 `steady_timer` 一次性到期等待（`ws_session.cpp:855-867`）：`pendingOfflineMsgs_` 为空则立即返回；超时/异常取消且仍有 pending → 告警日志 + `onDisconnect()`（保留数据库回执供重登恢复）。
8. **ACK 处理**：`AckReq.ackType` 只接受 `1`（在线）或 `2`（离线），否则**静默丢弃**（`ws_session.cpp:871-872`）。逐条 `push_client_->AckMsg(userID=account, serverMsgID, ackType)`，最多 **2 次尝试**，第 1 次失败后等 **200ms** 再试（`873-891`）；两次都失败 → 直接 `close` 底层 socket 并 `co_return`（`892-896`）。成功则从 `pendingOfflineMsgs_` 删除，若删空则 `batch_ack_timer_->cancel()`（`897-902`）。
9. **完成通知**：扫描正常读到空页且所有 ACK 已持久化，才发 `type=209, errorCode="0", errorMsg="offline sync complete"`（`ws_session.cpp:797-802`）。若中途抛异常 → 发 `type=209, errorCode="SYNC_ERROR"`（`342-349`）。
10. ACK 帧本身**没有任何响应**（`handleAck` 无回写），客户端靠 `209` 判断本轮结束。

注意：`pendingOfflineMsgs_` 只跟踪**离线重放**的 50 条/批；在线 `104` 推送的 ACK 不在本进程跟踪，直接转发给 PushService（`push_service/server.cpp:164-194` → `MySQLHandler::QueueMessageAck`）。

### 4.5 断线清理

`onDisconnect()`（`ws_session.cpp:906-934`）幂等（`disconnected_` 短路）且顺序固定：

1. `disconnected_ = true`；
2. `batch_ack_timer_->cancel()`；
3. `pendingOfflineMsgs_.clear()`；
4. `write_queue_->cancel(); write_queue_->close();`
5. `beast::get_lowest_layer(ws_)` 上 `cancel → shutdown(both) → close`（**不发 WS close 帧**）；
6. `account_ == 0`（未认证）直接返回，不碰 Redis；
7. `unregisterSession(account_, shared_from_this())`；返回 `false`（stale）则**不再改 Redis 在线状态**；
8. 否则 `setex("user:{account}:online", 300, "0")`。

触发点共 5 处：`start()` 里 `doRead` 抛异常（`84-85`）、`doReadLoop` 退出（`711` 及 `328` 的 spawn 尾）、写队列 drain 结束（`746`）、`asyncSend` 入队失败（`721-723`）、`queuedWrite` 入队失败（`778`）、`waitOfflineBatch` 超时（`866`）。**没有**服务端主动踢人/多端互斥逻辑。

### 4.6 并发 / strand 模型

- `WebServer(4, 6666, ...)`：`IOC_Pool` 4 个 `io_context`，每个一条线程（`ioc_pool.h:29-33`）；accept 用**独立的单线程** `_acceptor_ioc`（`webserver.cpp:47`），`listen()` 在该 ioc 上跑（`98`）。
- 每个连接：`make_strand(ioc[nextWorker])`，`nextWorker` round-robin（`webserver.cpp:126-127`）。`async_accept(executor, ...)` 把 socket 绑定到该 strand，随后 `ssl::stream` 继承同一 executor，因此 **`ws_.get_executor()` 就是该连接的 strand**。
- 所有 session 内部状态（`account_`、`disconnected_`、`pendingOfflineMsgs_`、`battle_ack_timer_`、`write_queue_`）只在 strand 上访问，**无锁**；`asyncSend` 通过 `asio::post(ws_.get_executor(), ...)` 回到 strand（`ws_session.cpp:717`）。
- 跨 strand 的共享状态只有 `WSSessionManager::sessions_`，用 `shared_mutex` 保护。
- `IOC_Pool::spawn()` 提供了"选活跃任务最少的 worker"的负载均衡（`ioc_pool.h:53-77`），但 **`WebServer::listener` 没用它**，用的是 `get_ioc(nextWorker)` 轮询。
- gRPC 回调（`GatewayPushServer::PushToUser`）在 gRPC 自己的线程上执行，通过 `pushToUser → asyncSend → post` 投递到目标 session 的 strand。

---

## 5. GatewayPushService

### 5.1 接口语义

`proto/gateway_push.proto:7-19`：

```
rpc PushToUser(PushToUserReq) returns (PushToUserResp)
PushToUserReq  { uint64 account = 1; bytes msgDataBin = 2; string conversationID = 3; }
PushToUserResp { bool success = 1; }
```

服务端实现（`gateway_push_service/server.cpp:11-53`）：

1. `account == 0` → `response.success=false` + gRPC `INVALID_ARGUMENT "account is required"`（`21-27`）。
2. 组装 `sdkws::SdkWSResp{ type=104, account=account, data=msgDataBin }`（`33-36`）。
3. `WSSessionManager::instance().pushToUser(account, payload)`（`41`）。
4. `response.success = pushed`（`43`）；`pushed==false` 时日志 `"push to user={} failed (no active session)"`，但 gRPC 状态仍是 `OK`（`52`）。

### 5.2 如何定位 session

**仅内存查表**：`sessions_.find(account)`（`ws_session_manager.cpp:52-59`）。没有 Redis 路由、没有跨实例转发、没有 `deviceID` 维度。命中后 `session->asyncSend(payload)` → `asio::post` 到该 session 的 strand → `write_queue_->try_send`（256 容量，`ws_session.cpp:731-733`）。

### 5.3 语义边界（重要）

- `success=true` 只代表"入队成功"，**不代表已写出、更不代表客户端已收到**。`asyncSend` 的失败路径是**关闭连接**（`ws_session.cpp:719-724`），不回报调用方。
- `msgDataBin` **不做任何反序列化校验**，直接塞进 `SdkWSResp.data`；`conversationID` **完全未使用**（`gateway_push_service/server.cpp` 全文无 `conversationid()` 引用）。
- 下推帧无 `requestId`、无 `errorCode`、无 `deviceID`。

### 5.4 谁在调用

`PushService` 的单聊链路：`push_msg_handler` → `PushServer::PushMsg` → 先查 `"user:{recvID}:online"`，在线则 `pushClient->PushToUser(recvID, msgData.SerializeAsString(), conversationID)`（`push_service/server.cpp:116-149`）。

此外存在**第二条投递路径**：`GroupService` 直接在进程内调用 `WSSessionManager::instance().pushToUser(...)` 发 `type=251`（`group_service/server.cpp:45-50, 75-77`），完全绕过 `GatewayPushService` 的 gRPC 接口。

### 5.5 多实例限制

- `WSSessionManager` 是进程内单例（`ws_session_manager.cpp:7-10`），**Gateway 实例 A 看不到实例 B 的连接**。
- etcd 中 `GatewayPushService` 只注册了一个 `ipport`（`main.cpp:146-149` + `base_service.h:165-168`），而 `PushService::getGatewayPushClient()` **根本没走服务发现**：硬编码 `"localhost:50055"` 作 fallback，并在有 `BackbonClient` 时只打一条 `warn` 说"GetServicesList 是异步的需要 co_await"（`push_service/server.cpp:205-227`）。
- 结论：**当前实现等价于单实例网关**。多实例部署下，PushService 会把所有推送都打到 `localhost:50055`，其余实例上的用户必然 `success=false`；且失败后只能靠登录重放恢复。水平扩容能力**未实现**。

---

## 6. 错误码清单

### 6.1 `SdkWSResp.errorCode`（网关 → 客户端）

| errorCode | 含义/触发条件 | 位置 |
|---|---|---|
| `""`（空串） | 注册成功、认证成功、好友通知成功、`250` 成功 | `ws_session.cpp:163, 303, 697, 586-590` |
| `"0"` | 成功：心跳 pong、发消息成功、ACK 通知、`244`/`240-243` 默认、`200` 离线帧、`209` 完成帧、`230` 通知 | `481, 529, 600, 625, 697, 799, 842` |
| `"1"` | 首帧 Protobuf 解析失败（沿用了"错误码 1"，属历史值） | `ws_session.cpp:411` |
| `"401"` | 密码登录失败（`LoginRequest` 返回 `success=false`），`errorMsg = "login failed: " + reason` | `ws_session.cpp:219-220` |
| `"4401"` | 不支持的请求 type（首帧阶段），随后 `close(4404)` | `ws_session.cpp:360` |
| `"500"` | `SendMessages` 抛异常（`errorMsg = "SendMessages failed: " + e.what()`） | `ws_session.cpp:542-543` |
| `"INVALID_PARAM"` | ① 认证缺 `account`（`187`）；② token 无效/过期（`267`）；③ `250` 群命令解析失败（`579`）；④ `244` 历史请求非法（`603`）；⑤ `220-225` 好友命令解析失败（`664`） | 同左 |
| `"PARSE_ERROR"` | 注册帧 `data`（`RegisterRequest`）解析失败 | `ws_session.cpp:105` |
| `"IMCOMPLETE_INFO"` | 注册缺少 `phone/nickname/password` | `ws_session.cpp:126` |
| `"REG_CLIENT_ERROR"` | 网关调 AuthService `Register` 抛异常（`errorMsg = "Registration outcome unknown; retry the same request ID"`） | `ws_session.cpp:153-154` |
| `"REGISTER_FAILED"` | AuthService 返回失败但 `errorCode` 为空时的兜底 | `ws_session.cpp:167` |
| `"SYNC_ERROR"` | 离线消息重放中途异常，发 `type=209` 失败帧 | `ws_session.cpp:346` |
| `"HISTORY_ERROR"` | `GetSingleChatHistory` 抛异常 | `ws_session.cpp:611` |
| `"CONVERSATION_ERROR"` | 会话服务缺失或调用异常（`errMsg = e.what()`） | `ws_session.cpp:652` |
| `"SERVICE_UNAVAILABLE"` | 好友服务未注入（`friend_client_==nullptr`）或 `Execute` 抛异常 | `ws_session.cpp:667, 676` |
| `"DB_ERROR"` | 群服务不可用且 `group_client_` 为空 | `ws_session.cpp:584` |

### 6.2 内嵌在 `data` 里的错误码（`groupchat.Reply` / `friendpb.Reply`）

| errorCode | 场景 | 位置 |
|---|---|---|
| `"0"` | 成功（`250`/`220-225` 的 `data` 内） | `mysqlhandler.cpp:926, 1830`；`friend` 侧由 `ExecuteFriendCommand` 返回 |
| `"DB_ERROR"` | 好友数据暂不可用 / 群落库失败 | `mysqlhandler.cpp:930, 2039`；`group_service/client.cpp:19` |
| `"INVALID_PARAM"` | 请求解析失败（`250`/`220-225`） | `ws_session.cpp:579, 664` |

注：`250` 分支**不把** `Reply.errorCode` 复制到 `SdkWSResp.errorCode`，所以只看外层会全是空串（见 §2.3 补充）。

### 6.3 `AuthService` gRPC 响应的 `errorCode`（`AuthRegResponse.errorCode`）

| errorCode | 触发条件 | 位置 |
|---|---|---|
| `"INVALID_INFO"` | 注册参数不合法（requestId/phone/nickname/password 任一不合规） | `auth_service/server.cpp:97` |
| `"PHONE_EXISTS"` | 手机号已注册 | `mysqlhandler.cpp:463, 512` |
| `"ACCOUNT_RANGE_EXHAUSTED"` | 账号段耗尽 | `mysqlhandler.cpp:570` |
| `"ACCOUNT_POOL_UNAVAILABLE"` | 无可用账号，可同请求 ID 重试 | `mysqlhandler.cpp:612` |
| `"DB_ERROR"` | 注册过程中任意非 `RegistrationError` 的 `std::exception` | `auth_service/server.cpp:135` |
| `"INNER_ERROR"` | 注册未知异常 / 无法启动协程 | `auth_service/server.cpp:138, 146` |
| `"REGISTRATION_UNCERTAIN"` | 客户端侧：gRPC `!status.ok()`（超时等），"retry the same request ID" | `auth_service/client.cpp:68-74` |
| `"INVALID_PARAM"` | 登录 `account==0` 或 `password` 为空 | `auth_service/server.cpp:166` |
| `"USER_NOT_FOUND"` | 账号不存在（`errMsg = "Invalid username"`） | `auth_service/server.cpp:185` |
| `"WRONG_PASSWORD"` | 哈希不匹配（`errMsg = "Invalid account or password"`，未泄露账号是否存在） | `auth_service/server.cpp:199` |
| `"INTERNAL_ERROR"` | 登录协程抛异常（同时 gRPC status `INTERNAL`，`errMsg = e.what()`） | `auth_service/server.cpp:235-238` |

### 6.4 非 errorCode 的错误表达

| 形式 | 值 | 位置 |
|---|---|---|
| gRPC status | `INVALID_ARGUMENT "account is required"` | `gateway_push_service/server.cpp:24-26` |
| gRPC status | `INVALID_ARGUMENT "Invalid acknowledgement type"` | `push_service/server.cpp:176` |
| gRPC status | `UNIMPLEMENTED "Use GroupService Ack for group messages"` | `push_service/server.cpp:182` |
| gRPC status | `UNAVAILABLE "ACK persistence unavailable"` | `push_service/server.cpp:190` |
| gRPC status | `UNIMPLEMENTED "Offline ACK tracking uses durable MySQL receipts"`（`AddPendingOfflineAck` 整接口废弃） | `push_service/server.cpp:201` |
| gRPC status | `UNIMPLEMENTED "Use GroupService Execute for group messages"` | `push_service/server.cpp:109-110` |
| WS close code | `policy_error`(1008) / `4404` / 无 close 帧直接关 socket | 见 §2.4 |

### 6.5 观察到的编码不一致

1. **解析失败用 4 种不同码**：`"1"`（首帧，`411`）、`"PARSE_ERROR"`（注册 data，`105`）、`"INVALID_PARAM"`（群/好友/历史 data，`579/664/603`）。
2. **`INVALID_PARAM` 一词多义**：既表示"缺 account"也表示"token 无效"，客户端无法区分是否该重新登录（`187` vs `267`）。
3. **缺 `requestId` 的失败帧**：`account==0` 的 `INVALID_PARAM` 分支没有 `set_requestid`（`ws_session.cpp:186-190`），客户端无法关联；`doRead` 的解析失败帧同理（`410-413`）。
4. 数字码（`"1"/"401"/"4401"/"500"`）与字符串码（`"INVALID_PARAM"/"SYNC_ERROR"`）混用，无统一枚举。

---

## 7. 已知问题（仅列代码中确认的）

> `docs/gateway_issues.md` 中的 P1（同步 IO 阻塞）、P4（`async_close` fire-and-forget）、P5（离线 key 无 TTL）、P7（`start()` 生命周期）在**当前代码中已不成立**：gRPC 走 `ape::grpc_util::GrpcAwait`、Redis/Mongo 均为 `co_await` 异步；`onDisconnect` 已改为直接关底层 socket（`ws_session.cpp:913-917, 933`）；离线 key 用 `setex(...,300,"0")`（`928`）；`start()` 的 `self` 捕获确实保持了生命周期。以下只列我在代码里直接确认的问题。

### P0 — 会影响正确性

1. **`type=101` 双重语义**。认证前 = 登录（`ws_session.cpp:428-430`），认证后 = 发消息（`502`）；且两者的**响应 `type` 都是 101**（`306` vs `532`）。协议层无法自描述，任何"先缓存首帧再分发"的改造都会踩坑。
2. **未实现的 type 在认证后是"黑洞"**。`102/103/105` 及其它未知 type 在 `doReadLoop` 中只打 `debug` 日志、**不发任何响应也不关连接**（`ws_session.cpp:705-708`）。客户端若等待响应会永久挂起。而首帧阶段的未知 type 会直接 `close(4404)`（`372`）——两个阶段行为不一致。
3. **同账号重复登录不断开旧连接**。`registerSession` 直接覆盖 map（`ws_session_manager.cpp:22-23`），旧 session 既没被 `close`/踢下线，也不会再收到推送；其后续 `onDisconnect` 因 `it->second != session` 被判 stale，**不会把在线 key 置 0**（`ws_session_manager.cpp:38-47` + `ws_session.cpp:920-923`）。结果：用户已换新连接，但旧连接仍在消耗资源、且旧连接断开时不清在线状态。
4. **`SdkWSReq.lastMsgSeq` 完全未使用**。离线拉取恒从 `after = 0` 开始（`ws_session.cpp:811`），即使客户端声明了本地最大 seq 也要全量重新扫描并靠 MySQL 回执逐条去重。大账号登录时代价高。
5. **`handleAck` 未填 `AckMsgReq.seq`**。`push.proto:27` 注明 `seq` 用于更新 last_seq，但网关只设 `userID/serverMsgID/ackType`（`ws_session.cpp:877-881`），`seq` 恒为默认 0。
6. **`handleAck` 逐条 RPC**，N 条 ACK = N 次 gRPC；失败重试只有 1 次、退避仅 200ms（`ws_session.cpp:873-891`）。`push.proto` 没有批量 ACK 接口（`AckMsgReq` 只有单个 `serverMsgID`）。
7. **写失败导致断连但调用方不知情**。`asyncSend` 的 `try_send` 失败只在本 session 内 `onDisconnect`（`ws_session.cpp:719-724`），而 `GatewayPushService` 在此之前已经把 `success=true` 返回给 PushService（`gateway_push_service/server.cpp:41-43`）。PushService 会认为"网关已接受"，实际客户端没收到；虽然后续靠离线恢复兜底，但中间态无日志关联。

### P1 — 可观测的正确性/运维缺陷

8. **注册成功后连接语义不完整**。`handleReg` 回 `type=0`、不签发 token、不进入认证态（`ws_session.cpp:157-175`）；`start()` 只在 `doRead` **抛异常**时才 `onDisconnect()`（`84-85`），正常返回则协程结束、`shared_ptr` 释放、由 `~WSSession` 隐式析构 `ws_` 关闭 socket —— **不发 WebSocket close 帧**，客户端只能看到 TCP 断开。
9. **无服务端心跳**。只有客户端 `type=0` 才续期 Redis 在线 key（`ws_session.cpp:490-497`）；服务端不发 ping、不检查在线 key 与连接的一致性。客户端静默掉线（NAT 超时）时，在线状态最长滞留 300 秒。
10. **`GatewayPushService` 不校验载荷**。`msgDataBin` 直接透传，不做 `sdkws.MsgData` 反序列化校验（`gateway_push_service/server.cpp:19, 36`）；`conversationID` 声明了但从未使用。
11. **推送帧缺 `requestId`/`errorCode`**。`104`（`gateway_push_service/server.cpp:33-36`）与 `200`（`ws_session.cpp:841-848`，`requestId` 被塞成 `serverMsgID`）都无法让客户端做请求-响应关联。
12. **多实例推送未实现**。见 §5.5：进程内单例 map + `PushService` 硬编码 `localhost:50055`（`push_service/server.cpp:211-224`）。
13. **`WebServer::stop()` 无优雅关闭**。`stopPool()` 先 `work_guard.reset()` 再 `io_context.stop()` 再 `join`（`ioc_pool.h:35-50`），会丢弃 pending handler；`acceptor_.close()` 放在 `stop()` 最后（`webserver.cpp:102-106`）。没有任何"等待 session 自然关闭"的 deadline。
14. **`signal_set` 是 `start()` 局部变量**。`boost::asio::signal_set signals(get_acceptor_ioc(), SIGINT, SIGTERM)` + `signals.async_wait([&](auto, auto){ stop(); })`（`webserver.cpp:90-91`）。已核对：lambda 体只调用 `stop()`，不访问 `signals`，**不构成实际 UAF**；但 `start()` 返回后 `signals` 析构会 cancel 掉 handler，属脆弱写法。
15. **TLS 私钥/证书绝对路径硬编码**（`webserver.cpp:54-55`），且加载失败导致进程以退出码 0 静默退出（`main.cpp:220-223`）。
16. **无 URL 路径 / Origin / 子协议校验**（见 §2.1），任何路径都能建立连接。

### P2 — 认证层缺陷

17. **`ValidateToken` 不续期 TTL**（`auth_service/server.cpp:264-312`），token 固定 7 天硬过期；且**没有登出/吊销接口**（`apeauth.proto` 无对应 rpc）。
18. **密码哈希强度不足**：单轮 `SHA-256(salt||password)`（`auth_service/server.cpp:37-54`），无 KDF/迭代；`password_hash` 用 `!=` 普通字符串比较（`auth_service/server.cpp:197`），非恒定时间。
19. **盐质量**：`UUID字符串.substr(0,32)`，含 4 个连字符（`auth_service/server.cpp:56-59`），实际熵 < 128 bit。
20. **Redis 故障 ≡ token 无效**：`AuthClient::ValidateToken` 在 `!status.ok()` 时直接返回 `valid=false`（`auth_service/client.cpp:19-23`），网关据此回 `INVALID_PARAM "invalid or expired token"` 并关连接——Redis 抖动会导致全体重连失败，且错误码误导。
21. **注册与登录错误码不对齐**：网关侧 `handleReg` 兜底 `REGISTER_FAILED`（`ws_session.cpp:167`）与客户端侧 `REGISTRATION_UNCERTAIN`（`auth_service/client.cpp:71`）、服务端 `REG_CLIENT_ERROR`（`ws_session.cpp:153`）三套"未知结果"码并存。

### 代码卫生

22. `WSSession::write_drain_running_` 声明后**从未使用**（`include/gateway/ws_session.h:109`，全仓仅此 1 处命中）。
23. `WebServer::getAuthClient/getMsgClient/getPushClient/getGroupClient` 四个 public getter 无任何调用者（`webserver.h:52-55`）——连接参数实际由 `listener()` 直接传裸指针（`webserver.cpp:134-138`）。
24. `WSSessionManager::onlineAccounts()` 只被 `GroupService::Deliver()` 使用（`group_service/server.cpp:71`），与网关职责耦合；且 `GroupService` 直接 `#include "gateway/ws_session_manager.h"`（`group_service/server.cpp:2`）——同一进程中两个"微服务"共享内存状态，与 gRPC 解耦设计相矛盾。

---

### 附：核心事实速查

- 认证帧：`type=100` 注册、`type=101`（认证前）登录；心跳 `type=0`；发消息 `type=101`（认证后）；ACK `type=106`；离线单条 `type=200`；离线完成 `type=209`；在线下推 `type=104`；好友通知 `type=230`；群刷新 `type=251`。
- token = UUID 字符串，Redis `token:{uuid}` → `"{user_id},{account}"`，TTL 604800s，不续期，非 JWT。
- 在线 key = `user:{account}:online`，TTL 恒 300s，值 `"1"`/`"0"`。
- 离线批大小 50，ACK 等待超时 60s，ACK 重试 2 次 / 退避 200ms。
- 网关 IOC 池 4 worker × 1 线程，连接级 strand，round-robin 分配。
- 端口：50051 Auth / 50052 Backbon / 50053 Msg / 50054 Push / 50055 GatewayPush / 50056 Group / 50057 Friend / 50058 Conversation / 6666 WSS。

---

## 附录：Gateway 历史问题清单

> 基于对 `include/gateway/` 和 `src/gateway/` 下 5 个文件（webserver.h/cpp, ws_session.h/cpp, ws_session_manager.h/cpp）的完整扫描。
> 已修复项：ACK 忙等轮询已改用 `asio::experimental::channel` 事件驱动。
>
> **状态（2026-11-21）**：本文为历史记录，不再是待办清单。其中 **P1**（协程内同步 IO）、**P4**（异步关闭不完整）、**P5**（离线 key 无 TTL）、**P7**（`start()` 生命周期）**已在当前代码中修复或不再成立**；P5 涉及的 `user:{account}:online` 将在多网关改造中整体删除。仍然有效的条目：**P3**（多实例 Session）、**P6**（批量 ACK）、**P8**（`stopPool` 丢 pending）、**P9**（服务发现阻塞启动）、**P10**（`signal_set` 局部变量）。
> 当前待办以《多网关改造方案》（PR-1～PR-8）与《后端修改建议》为准。

---

### P1 — 协程线程上同步 IO 调用阻塞 Strand（最严重）

**问题**：gRPC、Redis、MongoDB 调用均在协程 strand 线程上**同步**执行。一次慢调用（500ms+）将卡住该 io_context 上**所有** WebSocket 连接的读写和心跳。

| 文件:行 | 位置 | 阻塞调用 | 影响范围 |
|---------|------|---------|---------|
| `ws_session.cpp:113` | `doReadAuth` | `auth_client_->ValidateToken(token)` （gRPC sync） | 认证阶段阻塞当前连接 |
| `ws_session.cpp:157-159` | `doReadAuth` | `redis.setex(...)` （Redis sync） | 阻塞 doReadAuth 协程 |
| `ws_session.cpp:263-264` | `doReadLoop` 心跳 | `redis.setex(...)` （Redis sync） | **所有连接**在同一 strand 上无法处理心跳/读帧 |
| `ws_session.cpp:293` | `doReadLoop` type=101 | `msg_client_->SendMessages(...)` （gRPC sync） | 发送消息时阻塞同连接后续帧 |
| `ws_session.cpp:386-389` | `pullAndPushOfflineMsgs` | `redis.get(...)` （Redis sync） | 离线拉取阶段阻塞 |
| `ws_session.cpp:399` | `pullAndPushOfflineMsgs` | `RedisHandler::GetOfflineMsgs(...)` （Redis 批量 sync） | 每批 50 条阻塞 |
| `ws_session.cpp:404-405` | `pullAndPushOfflineMsgs` | `MongoHandler::GetOfflineMsgsFromMongo(...)` （Mongo sync） | 冷存储回源时严重阻塞 |
| `ws_session.cpp:509` | `handleAck` | `RedisHandler::AckOfflineMsg(...)` （Redis sync） | 每条 ACK 阻塞 |
| `ws_session.cpp:513-517` | `handleAck` | `redis.get/set` （Redis sync） | 更新 last_seq 阻塞 |
| `ws_session.cpp:543` | `handleAck` for 循环内 | `push_client_->AckMsg(...)` （gRPC sync × N） | **N 次 gRPC** 阻塞，N=ACK 消息数 |

**建议方案**：
- **gRPC**：使用 `grpc::CompletionQueue` + 异步回调，或 `grpc::AsyncClient` API
- **Redis/MongoDB**：使用各自的 async driver（如 `boost::redis`、`mongocxx driver async`），或将阻塞调用投递到专用 `thread_pool`，通过 `asio::post` + `asio::use_awaitable` 桥接回 strand：
  ```cpp
  auto resp = co_await asio::post(io_pool_, asio::use_awaitable,
      [&] { return redis.get(key); });
  ```

---

### P2 — WebServer `listener` 子协程的正确 executor dispatch 问题

**位置**：`webserver.cpp:110-111`

```cpp
_iocPool.spawn(handle_ws_session(std::move(socket), ...));
```

`handle_ws_session` 返回 `awaitable<void>`，但它是在 `_acceptor_ioc` 的协程中创建的。`IOC_Pool::spawn` 通过 `co_spawn(ioc, ...)` 将其调度到选中的 worker io_context。**这是正确的**——`co_spawn` 的 executor 参数决定了协程在哪个 io_context 上执行。但需确认 `WSSession::ws_` 的底层 socket 的 executor 与 spawn 的 ioc 相同（通过 `ws_.get_executor()` 内联的 co_spawn 已正确使用）。

**状态**：当前实现正确，无明显 bug。

---

### P3 — Gateway 水平扩容 Session 管理缺失

**问题**：`WSSessionManager` 是**进程内存单例**（`static WSSessionManager mgr`），Gateway 实例 A 不知道 Gateway 实例 B 上的用户连接。

**影响**：
- `GatewayPushService` 调用 `WSSessionManager::hasSession()` 只查本地，多实例部署下推送可能丢失
- PushService 通过 etcd 路由到任意一个 Gateway 实例，若用户不在该实例上连接，`pushToUser` 返回 false

**建议方案**：
- Redis 记录 `user:{userId}:gateway_instance` → 当前连接的 Gateway 实例地址
- 跨实例推送时，接收方 Gateway 通过内部 HTTP/gRPC 转发

**相关文件**：
- `include/gateway/ws_session_manager.h`
- `src/gateway/ws_session_manager.cpp`

---

### P4 — `onDisconnect` 中 WebSocket 异步关闭不完整

**位置**：`ws_session.cpp:575-580`

```cpp
ws_.async_close(
    websocket::close_code::normal, [](boost::system::error_code ec) {
      if (ec) {
        spdlog::debug("WSSession: graceful close error: {}", ec.message());
      }
    });
```

**问题**：
1. fire-and-forget：handler 可能在 `io_context.stop()` 之后才执行，TCP 未优雅关闭
2. 若 `async_close` 需要写帧但连接已半关，可能静默失败

**建议**：使用 `co_await` 等待关闭完成，或在 server shutdown 时给 deadline：
```cpp
boost::system::error_code ec;
ws_.async_close(websocket::close_code::normal,
                asio::redirect_error(asio::use_awaitable, ec));
// 或 at least 使用 io_context 级别的 work tracking
```

---

### P5 — Redis 离线状态 key 无 TTL，永不删除

**位置**：`ws_session.cpp:569`

```cpp
redis.set("user:" + userId_ + ":online", "0");
```

**问题**：
- 在线 key 有 5 分钟 TTL：`redis.setex("user:" + userId_ + ":online", 300, "1")`
- 离线 key **无 TTL**：`redis.set("user:" + userId_ + ":online", "0")` 永久驻留
- 环境不一致：server 崩溃后 key 只存活 5 分钟（TTL），但正常离线设置的 key 永远不删

**建议**：统一使用 `setex` 并设合理 TTL（如 600 秒），或离线时直接 `DEL` key：
```cpp
redis.setex("user:" + userId_ + ":online", 600, "0");
// 或
redis.del("user:" + userId_ + ":online");
```

---

### P6 — `handleAck` 中 for 循环内逐条阻塞 gRPC

**位置**：`ws_session.cpp:536-547`

```cpp
for (int i = 0; i < ackReq.servermsgids_size(); ++i) {
    ...
    push_client_->AckMsg(ackMsgReq);  // 同步 gRPC，每次调用 ~ms 级延迟
}
```

**问题**：ACK 50 条消息 = 50 次同步 gRPC 调用，累计延迟显著。且 `handleAck` 从 `doReadLoop` 同步调用，直接阻塞消息读循环。

**建议**：
1. 批量 ACK 接口：修改 Proto 使 PushService 支持 `AckBatch` 一次 RPC 提交所有 msgId
2. 或异步 fire-and-forget 投递到线程池发送

---

### P7 — `start()` 中 lambda 捕获 `self` 的生命周期误用

**位置**：`ws_session.cpp:32-56`

```cpp
auto self = shared_from_this();
asio::co_spawn(
    ws_.get_executor(),
    [self]() -> asio::awaitable<void> { ... co_await self->doReadAuth(); },
    asio::detached);
co_return;
```

**问题**：`start()` 在 co_return 后，调用者（`handle_ws_session`）也 co_return，`session` 的 `shared_ptr` 引用计数减少。但 co_spawn 的 lambda 持有了 `self`，所以 session 不会提前析构。**这是正确的**。但 `start()` 本身是 `co_spawn` 的入口——它立即 `co_return` 而真正的逻辑在内部 lambda 中，这是双层 co_spawn，略显冗余但没有逻辑错误。

---

### P8 — IOC_Pool `stopPool` 执行顺序风险

**位置**：`webserver.h:61-76`

```cpp
void stopPool() {
    for (auto &w : _workers) {
      w->work_guard.reset();       // 1. 释放 work_guard
    }
    for (auto &w : _workers) {
      w->io_context.stop();        // 2. 停止 ioc
    }
    for (auto &t : _threads) {
      if (t.joinable()) {
        t.join();                  // 3. 等待线程
      }
    }
}
```

**问题**：
1. `reset()` 后 `io_context` 没有未完成 handler 就自动 `run()` 返回——这是预期行为
2. 但 `io_context.stop()` 又额外调用一次，导致 pending 的 `async_close` handler 被丢弃（`operation_aborted`）
3. 这与 P4 相互作用：`onDisconnect` 中 fire-and-forget 的 `async_close` 可能在 stop 前未完成

**建议**：在 `stop()` 前给一个 deadline 等待 pending 操作完成：
```cpp
// 先停止 acceptor（不再接收新连接）
acceptor_.close();
// 等待所有 session 自然关闭（带超时）
// 再 stopPool()
```

---

### P9 — `discover_Service` 阻塞构造函数

**位置**：`webserver.cpp:56-58`

```cpp
discover_Service("AuthService", auth_addr);   // 同步 gRPC
discover_Service("MsgService", msg_addr);
discover_Service("PushService", push_addr);
```

**问题**：构造函数在启动阶段阻塞 3 次 etcd 查询。若 etcd 慢或不可达，服务启动延迟大。

**建议**：启动后异步发现 + 定期刷新，支持运行时切换下游地址。

---

### P10 — `start()` 中 signal_set 的引用捕获风险

**位置**：`webserver.cpp:76-77`

```cpp
boost::asio::signal_set signals(get_acceptor_ioc(), SIGINT, SIGTERM);
signals.async_wait([&](auto, auto) { stop(); });
```

**问题**：lambda 捕获 `[&]`（引用 this 的隐式成员）。`signals` 是 `start()` 的局部变量。当 `stop()` 调用 `get_acceptor_ioc().stop()` 后，`run()` 返回、`start()` 返回、`signals` 析构。此时 handler 如果还未被 cancel 完成，存在微小的 use-after-free 窗口。实际上 `stop()` 会 cancel 所有 pending handlers，但优雅退出的顺序依赖 cancellation 优先级。

**建议**：将 `signals` 提升为成员变量，或使用 `shared_ptr` 捕获。

---

### 总结

| 优先级 | 问题 | 影响 |
|--------|------|------|
| **P0** | 协程同步 IO 阻塞 (P1) | 整个 strand 卡死，所有连接受影响 |
| **P0** | handleAck for 循环内 N 次 gRPC (P6) | 批量 ACK 延迟累积 |
| **P1** | Gateway 多实例 Session 不感知 (P3) | 水平扩容后推送丢失 |
| **P1** | async_close fire-and-forget (P4) | TCP 可能未优雅关闭 |
| **P1** | stopPool 丢弃 pending handler (P8) | 关闭时清理不完整 |
| **P2** | Redis 离线 key 无 TTL (P5) | Redis 内存泄漏 |
| **P2** | discover_Service 阻塞构造 (P9) | 启动延迟 |
| **P2** | signal_set 局部变量捕获 (P10) | 极端情况 UAF |