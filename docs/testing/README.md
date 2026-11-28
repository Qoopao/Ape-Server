# 测试与验收

> 状态：现状  
> 权威性：本文是「测试策略、环境、入口、结果索引」的唯一来源  
> 最后核对：2026-11-26 @ `03ed7c2` + 工作区改造  
> 适用代码：`test/`、`scripts/smoke/`、`scripts/run_group_tests.sh`

## 1. 策略

| 层级 | 目标 | 入口 |
|---|---|---|
| 编译/静态 | 代码可编译、协议/类型一致 | `cmake --build build -j4` |
| 单元/集成 | 存储、协议、边界 | `test/` + CMake option |
| 冒烟 | 网关/多网关/群聊/ACK 主链路 | `scripts/smoke/` |
| 可观测 | trace 传播/指标 no-op/配置结构 | `observability_test` + `scripts/observability/` |
| 压测 | 长连接/吞吐/延迟 | `scripts/long_conn_test.sh` |

测试必须使用隔离环境；不得复用真实用户数据。冒烟脚本会 `pkill` 并清 etcd，见 [operations.md](../operations.md) §10。

可观测验收分三层：

```bash
# 1) 纯函数/no-op 单测（无中间件）
cmake -S . -B build -DAPE_OBSERVABILITY_TESTS=ON
cmake --build build --target observability_test -j8 && ./build/observability_test

# 2) 看板/告警/Collector 配置结构校验
python3 scripts/observability/validate_configs.py

# 3) 端到端：先发一条消息，再用 logID/serverMsgID 校验 Jaeger + VictoriaMetrics
python3 scripts/observability/observability_smoke.py --log-id <logID> --server-msg-id <serverMsgID>
```

契约与指标白名单见 [design/observability.md](../design/observability.md)。

## 2. 环境

| 组件 | 要求 |
|---|---|
| MySQL | `ape_auth`，用户 `ape_user/ape_password`；或测试用独立 MariaDB（`run_group_tests.sh` 使用 33079） |
| MongoDB | `IM-System`，`root/root` |
| Redis | 密码 `redis123` |
| etcd | `127.0.0.1:2379` |
| Kafka | 在线链路需要；离线/ACK 核心用例可不依赖 |
| Python | `protobuf` 运行时；部分用例需要 `pymongo` |

## 3. 单聊/ACK

```bash
bash scripts/smoke/start_ack.sh
python3 scripts/smoke/ack_test.py --json /tmp/ack-results.json
python3 scripts/smoke/ack_test.py --faults --case ACK-050
python3 scripts/smoke/ack_test.py --e2e --case ACK-001 --case ACK-041
```

用例与断言见 [messaging.md](messaging.md)。最近结果：[results/ack-2026-11-24.json](results/ack-2026-11-24.json)。

## 4. 群聊

```bash
# 独立 MariaDB + 集成测试（会下载 deb，需网络）
bash scripts/run_group_tests.sh

# 或在已就绪环境跑多网关群聊冒烟
bash scripts/smoke/start_services.sh
python3 scripts/smoke/group_test.py
```

用例见 [group-chat.md](group-chat.md)。

## 5. 多网关/网关

```bash
bash scripts/smoke/start_services.sh
python3 scripts/smoke/ws_test.py       # 注册/登录/209/路由/互踢/CAS/etcd
python3 scripts/smoke/group_test.py    # 群消息跨实例扇出
python3 scripts/smoke/drain_test.py    # SIGTERM -> SERVER_DRAINING + etcd 注销
bash scripts/smoke/start_all.sh
python3 scripts/smoke/all_test.py      # --role=all 兼容模式
```

## 6. 验收矩阵

| 变更类型 | 最低验证 |
|---|---|
| 纯文档 | 链接/路径检查 |
| 局部 C++ | 编译 + 相关单测/集成 |
| 网关/协议 | 编译 + `ws_test.py` + 手工帧 |
| 单聊/ACK | 编译 + `ack_test.py` |
| 群聊 | 编译 + `run_group_tests.sh` 或 `group_test.py` |
| 多网关 | `ws_test.py` + `group_test.py` + `drain_test.py` |
| schema | 编译 + 集成 + 手工 SQL（建表/幂等/回滚） |
| 性能 | `long_conn_test.sh` 或专用压测，给前后对比 |

## 7. 结果管理

- 结果 JSON 放 `testing/results/`，文件名带日期：`ack-YYYY-MM-DD.json`。
- 结果文件不手工改；附一句话说明命令、环境、通过/跳过/失败数。
- 失败用例必须保留命令与关键日志；不要用「环境问题」掩盖代码问题。

## 8. 相关文档

- [operations.md](../operations.md)
- [implementation/status.md](../implementation/status.md)
- [design/messaging.md](../design/messaging.md)
- [design/group-chat.md](../design/group-chat.md)
