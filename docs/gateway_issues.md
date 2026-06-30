# Gateway 代码未解决问题分析

> 基于对 `include/gateway/` 和 `src/gateway/` 下 5 个文件（webserver.h/cpp, ws_session.h/cpp, ws_session_manager.h/cpp）的完整扫描。
> 已修复项：ACK 忙等轮询已改用 `asio::experimental::channel` 事件驱动。

---

## P1 — 协程线程上同步 IO 调用阻塞 Strand（最严重）

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

## P2 — WebServer `listener` 子协程的正确 executor dispatch 问题

**位置**：`webserver.cpp:110-111`

```cpp
_iocPool.spawn(handle_ws_session(std::move(socket), ...));
```

`handle_ws_session` 返回 `awaitable<void>`，但它是在 `_acceptor_ioc` 的协程中创建的。`IOC_Pool::spawn` 通过 `co_spawn(ioc, ...)` 将其调度到选中的 worker io_context。**这是正确的**——`co_spawn` 的 executor 参数决定了协程在哪个 io_context 上执行。但需确认 `WSSession::ws_` 的底层 socket 的 executor 与 spawn 的 ioc 相同（通过 `ws_.get_executor()` 内联的 co_spawn 已正确使用）。

**状态**：当前实现正确，无明显 bug。

---

## P3 — Gateway 水平扩容 Session 管理缺失

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

## P4 — `onDisconnect` 中 WebSocket 异步关闭不完整

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

## P5 — Redis 离线状态 key 无 TTL，永不删除

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

## P6 — `handleAck` 中 for 循环内逐条阻塞 gRPC

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

## P7 — `start()` 中 lambda 捕获 `self` 的生命周期误用

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

## P8 — IOC_Pool `stopPool` 执行顺序风险

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

## P9 — `discover_Service` 阻塞构造函数

**位置**：`webserver.cpp:56-58`

```cpp
discover_Service("AuthService", auth_addr);   // 同步 gRPC
discover_Service("MsgService", msg_addr);
discover_Service("PushService", push_addr);
```

**问题**：构造函数在启动阶段阻塞 3 次 etcd 查询。若 etcd 慢或不可达，服务启动延迟大。

**建议**：启动后异步发现 + 定期刷新，支持运行时切换下游地址。

---

## P10 — `start()` 中 signal_set 的引用捕获风险

**位置**：`webserver.cpp:76-77`

```cpp
boost::asio::signal_set signals(get_acceptor_ioc(), SIGINT, SIGTERM);
signals.async_wait([&](auto, auto) { stop(); });
```

**问题**：lambda 捕获 `[&]`（引用 this 的隐式成员）。`signals` 是 `start()` 的局部变量。当 `stop()` 调用 `get_acceptor_ioc().stop()` 后，`run()` 返回、`start()` 返回、`signals` 析构。此时 handler 如果还未被 cancel 完成，存在微小的 use-after-free 窗口。实际上 `stop()` 会 cancel 所有 pending handlers，但优雅退出的顺序依赖 cancellation 优先级。

**建议**：将 `signals` 提升为成员变量，或使用 `shared_ptr` 捕获。

---

## 总结

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