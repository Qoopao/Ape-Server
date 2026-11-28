# 多网关冒烟测试

这些脚本用于验证多网关改造（`docs/多网关改造方案.md`）的关键链路：
路由表、跨实例互踢、CAS 自愈、群消息跨实例扇出、优雅下线、单进程兼容模式。

## 前置环境

本目录只包含"已就绪环境"上的测试；基础设施需自行提供（任选 Docker 或本机进程）：

| 组件 | 地址 | 凭据 |
|---|---|---|
| Redis | 127.0.0.1:6379 | 密码 `redis123` |
| etcd | 127.0.0.1:2379 | 无 |
| MySQL | 127.0.0.1:3306 | 库 `ape_auth`，用户 `ape_user` / `ape_password`（或 `--skip-grant-tables`） |
| MongoDB | 127.0.0.1:27017 | 用户 `root` / `root`，`authSource=admin` |

MySQL 表结构可用 `docker/mysql/init.sql` 初始化：

```bash
mysql -h127.0.0.1 -uroot ape_auth < docker/mysql/init.sql
```

Python 侧只需要 protobuf 运行时（`python3-protobuf`）与 `pymongo`（可选）。

## 运行

```bash
# 1) 起 backbon / auth / group / gw-1 / gw-2（各自独立进程）
bash scripts/smoke/start_services.sh

# 2) 基础多网关链路：注册/登录/209/路由/互踢/CAS/etcd 注册
python3 scripts/smoke/ws_test.py

# 3) 群聊 v3（246 发送 / 251 内联 / 210 群同步结束；无 ACK）
bash scripts/smoke/start_group.sh
python3 scripts/smoke/group_test.py

# 4) 优雅下线（PR-6）：SIGTERM -> SERVER_DRAINING + etcd 注销
python3 scripts/smoke/drain_test.py

# 5) 单进程兼容模式（--role=all）
bash scripts/smoke/start_all.sh
python3 scripts/smoke/all_test.py
```

测试会注册随机手机号账号、创建临时群，请只对开发环境执行。

群聊 v3 的事实以 `docs/group/` 为准：消息正文在 Mongo `IM-System.msg` 单副本，
`group_topic` 由 PushService 的 `push-group-fanout` 消费后经网关下发 251；
`start_group.sh` 因此必须包含 `--role=push`（GroupService 自己不再扇出）。

注意端口：`GroupService` 固定占用 50056，因此第二个网关实例的
`--push-port` 使用 50075，避免与本机 GroupService 冲突。部署时请为每个
网关实例分配不冲突的 `--push-port`。

## 单聊 ACK（`docs/ack`）

ACK（`type=106`）链路要经过 PushService，而 `start_services.sh` 只起
backbon/auth/group/gateway，所以 ACK 用单独的启动脚本：

```bash
# 1) backbon / auth / msg / push / gw-1 / gw-2（并把 Outbox 清理间隔调成 2s，便于验证 H 节）
bash scripts/smoke/start_ack.sh

# 2) ACK 验收（A–D/F/H；默认构造会话水位与 Mongo 文档，不依赖 Kafka）
python3 scripts/smoke/ack_test.py --json /tmp/ack-results.json

# 3) 故障注入：停止并重启 PushService（ACK-050）
python3 scripts/smoke/ack_test.py --faults --case ACK-050

# 4) 真实发送链路（慢，且需要 Kafka 可用）
python3 scripts/smoke/ack_test.py --e2e --case ACK-001 --case ACK-041
```

说明：

- `gen_proto.py` 现在同时生成 `msg.proto` / `conversation.proto`，供 244 拉历史用例使用；
- 用例清单与断言口径见 `docs/ack/测试用例与边界.md`，需求见 `docs/ack/README.md`；
- `--json` 输出每个用例的 PASS/SKIP/WARN/FAIL 与指标（`ACK-018.elapsed_ms` 等）；
- 环境里没有 Kafka 时，真实发送的 `op=1` 任务会在 `deliverConfirmed` 上等
  `delivery.timeout.ms`（10s）再退避重试，这是环境限制，不影响 ACK 本身。
