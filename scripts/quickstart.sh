#!/bin/bash

# 统一快速启动脚本
# etcd, kafka, redis, mongodb, mysql, otel 

set -e

# 颜色定义
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${GREEN}统一快速启动脚本${NC}"
echo "================================"

# 检查 Docker
if ! command -v docker &> /dev/null; then
    echo -e "${YELLOW}Docker 未安装，请先安装 Docker${NC}"
    exit 1
fi

# 检查 Docker Compose
if ! command -v docker-compose &> /dev/null && ! docker compose version &> /dev/null; then
    echo -e "${YELLOW}Docker Compose 未安装，请先安装 Docker Compose${NC}"
    exit 1
fi

# 检查 Docker 服务
if ! docker info &> /dev/null; then
    echo -e "${YELLOW}Docker 服务未运行，正在启动...${NC}"
    sudo systemctl start docker
fi

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DOCKER_DIR="$SCRIPT_DIR/docker"

# 显示菜单
show_menu() {
    echo ""
    echo -e "${BLUE}请选择操作:${NC}"
    echo "1) 启动 etcd (服务发现)"
    echo "2) 启动 kafka (消息队列)"
    echo "3) 启动 redis (缓存)"
    echo "4) 启动 mongodb (数据库)"
    echo "5) 启动 mysql (数据库)"
    echo "6) 启动 otel (OpenTelemetry监控栈)"
    echo "7) 启动所有服务"
    echo "8) 停止所有服务"
    echo "9) 查看服务状态"
    echo "0) 退出"
    echo ""
    read -p "请输入选择 (0-9): " choice
}

# 启动 etcd
start_etcd() {
    echo -e "${GREEN}启动 etcd...${NC}"
    cd "$DOCKER_DIR/etcd"
    if docker ps | grep -q "etcd-standalone"; then
        echo -e "${YELLOW}  etcd 已经在运行中${NC}"
    else
        ./deploy.sh standalone
    fi
    cd "$DOCKER_DIR"
}

# 启动 kafka
start_kafka() {
    echo -e "${GREEN}启动 kafka...${NC}"
    cd "$DOCKER_DIR/kafka"
    if docker ps | grep -q "kafka-standalone"; then
        echo -e "${YELLOW}  kafka 已经在运行中${NC}"
    else
        ./deploy.sh standalone
    fi
    cd "$DOCKER_DIR"
}

# 启动 redis
start_redis() {
    echo -e "${GREEN}启动 redis...${NC}"
    cd "$DOCKER_DIR/redis"
    if docker ps | grep -q "redis"; then
        echo -e "${YELLOW}  redis 已经在运行中${NC}"
    else
        ./deploy.sh standalone
    fi
    cd "$DOCKER_DIR"
}

# 启动 mongodb
start_mongodb() {
    echo -e "${GREEN}启动 mongodb...${NC}"
    cd "$DOCKER_DIR/mongodb"
    if docker ps | grep -q "mongo"; then
        echo -e "${YELLOW}  mongodb 已经在运行中${NC}"
    else
        ./deploy.sh standalone
    fi
    cd "$DOCKER_DIR"
}

# 启动 mysql
start_mysql() {
    echo -e "${GREEN}启动 mysql...${NC}"
    cd "$DOCKER_DIR/mysql"
    if docker ps | grep -q "mysql"; then
        echo -e "${YELLOW}  mysql 已经在运行中${NC}"
    else
        ./deploy.sh standalone
    fi
    cd "$DOCKER_DIR"
}

# 启动 OpenTelemetry 监控栈
start_otel() {
    echo -e "${GREEN}启动 OpenTelemetry 监控栈...${NC}"
    cd "$DOCKER_DIR/otel"
    if docker ps | grep -q "otel-collector"; then
        echo -e "${YELLOW}  OpenTelemetry 监控栈已经在运行中${NC}"
    else
        ./deploy.sh start
    fi
    cd "$DOCKER_DIR"
}

# 启动全部服务
start_all() {
    echo -e "${GREEN}启动所有服务...${NC}"
    start_etcd
    start_kafka
    start_redis
    start_mongodb
    start_mysql
    start_otel
}

# 停止所有服务
stop_all() {
    echo -e "${YELLOW}停止所有服务...${NC}"

    echo -e "${YELLOW}停止 etcd...${NC}"
    cd "$DOCKER_DIR/etcd" && ./deploy.sh stop 2>/dev/null || true

    echo -e "${YELLOW}停止 kafka...${NC}"
    cd "$DOCKER_DIR/kafka" && ./deploy.sh stop 2>/dev/null || true

    echo -e "${YELLOW}停止 redis...${NC}"
    cd "$DOCKER_DIR/redis" && ./deploy.sh stop 2>/dev/null || true

    echo -e "${YELLOW}停止 mongodb...${NC}"
    cd "$DOCKER_DIR/mongodb" && ./deploy.sh stop 2>/dev/null || true

    echo -e "${YELLOW}停止 mysql...${NC}"
    cd "$DOCKER_DIR/mysql" && ./deploy.sh stop 2>/dev/null || true

    echo -e "${YELLOW}停止 OpenTelemetry 监控栈...${NC}"
    cd "$DOCKER_DIR/otel" && ./deploy.sh stop 2>/dev/null || true

    cd "$DOCKER_DIR"
    echo -e "${GREEN}所有服务已停止${NC}"
}

# 查看服务状态
show_status() {
    echo -e "${BLUE}服务状态:${NC}"
    echo ""

    if docker ps | grep -q "etcd-standalone"; then
        echo -e "${GREEN}  etcd:      运行中 (localhost:2379, browser: http://localhost:2579)${NC}"
    else
        echo -e "${YELLOW}  etcd:      未运行${NC}"
    fi

    if docker ps | grep -q "kafka-standalone"; then
        echo -e "${GREEN}  kafka:     运行中 (localhost:29092, kafka-ui: http://localhost:9091)${NC}"
    else
        echo -e "${YELLOW}  kafka:     未运行${NC}"
    fi

    if docker ps | grep -q "redis"; then
        echo -e "${GREEN}  redis:     运行中 (localhost:6379, redis-commander: http://localhost:8088)${NC}"
    else
        echo -e "${YELLOW}  redis:     未运行${NC}"
    fi

    if docker ps | grep -q "mongo"; then
        echo -e "${GREEN}  mongodb:   运行中 (localhost:27017, mongo-express: http://localhost:8081)${NC}"
    else
        echo -e "${YELLOW}  mongodb:   未运行${NC}"
    fi

    if docker ps | grep -q "mysql"; then
        echo -e "${GREEN}  mysql:     运行中 (localhost:3306)${NC}"
    else
        echo -e "${YELLOW}  mysql:     未运行${NC}"
    fi

    if docker ps | grep -q "otel-collector"; then
        echo -e "${GREEN}  otel:      运行中 (grafana: http://localhost:3000, jaeger: http://localhost:16686)${NC}"
    else
        echo -e "${YELLOW}  otel:      未运行${NC}"
    fi

    echo ""
}

# 显示服务信息
show_info() {
    echo ""
    echo -e "${GREEN}服务启动完成！${NC}"
    echo ""
    echo "服务信息:"
    echo "  - etcd:           http://localhost:2379"
    echo "  - etcd-browser:   http://localhost:2579"
    echo "  - kafka:           localhost:29092"
    echo "  - kafka-ui:        http://localhost:9091"
    echo "  - redis:           localhost:6379"
    echo "  - redis-commander: http://localhost:8088"
    echo "  - mongodb:         localhost:27017"
    echo "  - mongo-express:   http://localhost:8081"
    echo "  - mysql:           localhost:3306"
    echo "  - grafana:         http://localhost:3000"
    echo "  - jaeger:          http://localhost:16686"
    echo "  - kibana:          http://localhost:5601"
    echo ""
    echo "管理命令:"
    echo "  docker/etcd/deploy.sh status"
    echo "  docker/kafka/deploy.sh status"
    echo "  docker/redis/deploy.sh status"
    echo "  docker/mongodb/deploy.sh status"
    echo "  docker/mysql/deploy.sh status"
}

# 主循环
while true; do
    show_menu

    case $choice in
        1)
            start_etcd
            show_info
            ;;
        2)
            start_kafka
            show_info
            ;;
        3)
            start_redis
            show_info
            ;;
        4)
            start_mongodb
            show_info
            ;;
        5)
            start_mysql
            show_info
            ;;
        6)
            start_otel
            show_info
            ;;
        7)
            start_all
            show_info
            ;;
        8)
            stop_all
            ;;
        9)
            show_status
            ;;
        0)
            echo "退出..."
            exit 0
            ;;
        *)
            echo "无效选择，请重新输入"
            ;;
    esac

    echo ""
done
