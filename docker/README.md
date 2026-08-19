# Docker 服务部署

本目录包含项目依赖的所有基础设施服务的 Docker 部署配置，每个服务均提供独立的 `deploy.sh` 管理脚本。

## 快速启动

```bash
# 启动全部服务（单机版）
cd ../scripts && ./quickstart.sh

# 或逐个启动
cd docker/etcd    && ./deploy.sh standalone
cd docker/kafka   && ./deploy.sh standalone
cd docker/redis   && ./deploy.sh standalone
cd docker/mongodb && ./deploy.sh standalone
cd docker/mysql   && ./deploy.sh standalone
cd docker/otel    && ./deploy.sh start
```

## 服务概览

| 服务 | 端口 | Web UI | 用户名/密码 |
|------|------|--------|-------------|
| etcd | 2379 | http://localhost:2579 | 无 |
| kafka | 29092 | http://localhost:9091 | root / root |
| redis | 6379 | http://localhost:8088 | admin / admin123 |
| mongodb | 27017 | http://localhost:8081 | root / root |
| mysql | 3306 | - | ape_user / ape_password |
| otel (grafana) | 3000 | http://localhost:3000 | 匿名登录 |
| otel (jaeger) | 16686 | http://localhost:16686 | 无 |
| otel (kibana) | 5601 | http://localhost:5601 | 无 |

---

## 1. etcd — 服务发现

**镜像**: `quay.io/coreos/etcd:v3.5.10`

**部署模式**:
- `standalone` — 单节点（开发环境）
- `cluster` — 3节点集群（生产环境）

**管理命令**:
```bash
./deploy.sh standalone   # 启动单机版
./deploy.sh cluster      # 启动3节点集群
./deploy.sh stop         # 停止
./deploy.sh status       # 查看状态
./deploy.sh test         # 测试连接
./deploy.sh clean        # 清理容器和数据
```

---

## 2. kafka — 消息队列

**镜像**: `apache/kafka:4.1.1`

**部署模式**:
- `standalone` — 单节点 KRaft 模式（开发环境）
- `cluster` — 3节点 KRaft 集群（生产环境）

**特性**:
- 启动后自动创建 `msg_topic` 和 `offline_msg_topic`（10分区）
- 自带 kafka-ui 管理界面

**管理命令**:
```bash
./deploy.sh standalone        # 启动单机版
./deploy.sh cluster           # 启动3节点集群
./deploy.sh stop              # 停止
./deploy.sh status            # 查看状态
./deploy.sh test              # 测试连接
./deploy.sh create-topic <名称> [分区数]  # 创建主题
./deploy.sh produce <主题> <消息>         # 生产消息
./deploy.sh consume <主题> [条数]         # 消费消息
./deploy.sh clean             # 清理容器和数据
```

---

## 3. redis — 缓存

**镜像**: `redis:7.2-alpine`

**部署模式**:
- `standalone` — 单节点（开发环境）

**连接**: `redis://localhost:6379`，密码 `redis123`

**管理命令**:
```bash
./deploy.sh standalone   # 启动单机版
./deploy.sh stop         # 停止
./deploy.sh status       # 查看状态
./deploy.sh test         # 测试连接
./deploy.sh backup       # 备份数据
./deploy.sh restore <文件>  # 恢复数据
./deploy.sh clean        # 清理容器和数据
```

---

## 4. mongodb — 文档数据库

**镜像**: `mongo:7.0`

**部署模式**:
- `standalone` — 单节点（开发环境）
- `replica` — 3节点副本集（高可用）
- `sharded` — 分片集群（3 shard + 3 config + 1 mongos）

**连接**: `mongodb://root:root@localhost:27017`

**管理命令**:
```bash
./deploy.sh standalone   # 启动单机版
./deploy.sh replica      # 启动副本集
./deploy.sh sharded      # 启动分片集群
./deploy.sh stop         # 停止
./deploy.sh status       # 查看状态
./deploy.sh test         # 测试连接
./deploy.sh backup       # 备份数据
./deploy.sh restore <目录>  # 恢复数据
./deploy.sh clean        # 清理容器和数据
```

---

## 5. mysql — 关系型数据库

**镜像**: `mysql:8.4`

**部署模式**:
- `standalone` — 单节点

**连接信息**:
- 地址: `localhost:3306`
- 数据库: `ape_auth`
- 用户: `ape_user` / `ape_password`
- Root: `root` / `ape_root_password`

**管理命令**:
```bash
./deploy.sh standalone   # 启动
./deploy.sh stop         # 停止
./deploy.sh status       # 查看状态
./deploy.sh test         # 测试连接
./deploy.sh backup       # 备份数据库
./deploy.sh restore <文件>  # 恢复数据库
./deploy.sh clean        # 清理容器和数据
```

---

## 6. otel — OpenTelemetry 监控栈

**包含组件**:

| 组件 | 端口 | 用途 |
|------|------|------|
| otel-collector | 4317 | OTLP gRPC 接收器 |
| jaeger-all-in-one | 16686 | 分布式追踪 UI |
| grafana | 3000 | 监控仪表盘 |
| victoriametrics | 8428 | 指标存储 |
| elasticsearch | 9200 | 日志/Span 存储 |
| kibana | 5601 | 日志查询 UI |

**管理命令**:
```bash
./deploy.sh start     # 启动
./deploy.sh stop      # 停止
./deploy.sh status    # 查看状态
./deploy.sh test      # 测试服务连接
./deploy.sh clean     # 清理容器和数据
```

> 注意: 监控栈容器较多，启动需要较长时间（约60秒），且对内存有一定要求。

---

## 目录结构

```
docker/
├── README.md
├── etcd/
│   ├── deploy.sh
│   ├── docker-compose-standalone.yml
│   └── docker-compose-cluster.yml
├── kafka/
│   ├── deploy.sh
│   ├── docker-compose-standalone.yml
│   └── docker-compose-cluster.yml
├── redis/
│   ├── deploy.sh
│   ├── docker-compose-standalone.yml
│   └── docker-compose-sentinel.yml
├── mongodb/
│   ├── deploy.sh
│   ├── docker-compose-standalone.yml
│   ├── docker-compose-replica.yml
│   ├── docker-compose-sharded.yml
│   └── *-init.js
├── mysql/
│   ├── deploy.sh
│   └── docker-compose-standalone.yml
└── otel/
    ├── deploy.sh
    ├── docker-compose.yml
    └── otel-collector-config.yaml
```
