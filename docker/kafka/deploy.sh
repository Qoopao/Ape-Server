#!/bin/bash

# Docker Kafka 部署管理脚本

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 日志函数
log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_debug() {
    echo -e "${BLUE}[DEBUG]${NC} $1"
}

# 脚本目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOCKER_DIR="$SCRIPT_DIR"

# 显示帮助信息
show_help() {
    echo "Docker Kafka 部署管理脚本"
    echo ""
    echo "用法: $0 [命令] [选项]"
    echo ""
    echo "命令:"
    echo "  standalone    启动单机版 Kafka (开发环境)"
    echo "  cluster       启动3节点 Kafka 集群 (生产环境)"
    echo "  stop          停止所有 Kafka 容器"
    echo "  restart       重启所有 Kafka 容器"
    echo "  status        查看 Kafka 容器状态"
    echo "  logs          查看 Kafka 日志"
    echo "  clean         清理所有 Kafka 容器和数据"
    echo "  test          测试 Kafka 连接"
    echo "  create-topic  创建测试主题"
    echo "  produce       生产测试消息"
    echo "  consume       消费测试消息"
    echo "  help          显示此帮助信息"
    echo ""
    echo "选项:"
    echo "  -f, --file    指定 docker-compose 文件"
    echo "  -d, --detach  后台运行"
    echo ""
    echo "示例:"
    echo "  $0 standalone          # 启动单机版 Kafka"
    echo "  $0 cluster             # 启动3节点集群"
    echo "  $0 status              # 查看状态"
    echo "  $0 test                # 测试连接"
    echo "  $0 create-topic test   # 创建测试主题"
}

# 检查 Docker 是否安装
check_docker() {
    if ! command -v docker &> /dev/null; then
        log_error "Docker 未安装"
        log_info "请先安装 Docker: https://docs.docker.com/get-docker/"
        exit 1
    fi

    # 检查 Docker Compose
    if ! command -v docker-compose &> /dev/null; then
        log_error "Docker Compose 未安装"
        log_info "请先安装 Docker Compose: https://docs.docker.com/compose/install/"
        exit 1
    fi

    # 检查 Docker 服务是否运行
    if ! docker info &> /dev/null; then
        log_error "Docker 服务未运行"
        log_info "请启动 Docker 服务: sudo systemctl start docker"
        exit 1
    fi
}

# 启动单机版 Kafka
start_standalone() {
    log_info "启动单机版 Kafka..."
    cd "$DOCKER_DIR"

    # 检查是否已经运行
    if docker-compose -f docker-compose-standalone.yml ps | grep -q "Up"; then
        log_info "单机版 Kafka 已经在运行中"
        log_info "Kafka 端点: localhost:29092"
        log_info "Kafka UI: http://localhost:9091"
        return 0
    fi

    # 准备数据目录
    mkdir -p kafka-standalone/data kafka-ui/config
    chown -R 1000:1000 kafka-standalone/data

    docker-compose -f docker-compose-standalone.yml up -d

    log_info "等待 Kafka 启动..."
    sleep 5

    local retry_count=0
    while [ $retry_count -lt 6 ]; do
        if docker-compose -f docker-compose-standalone.yml ps | grep -q "Up"; then
            log_info "单机版 Kafka 启动成功"
            log_info "Kafka 端点: localhost:29092"
            log_info "Kafka UI: http://localhost:9091"

            # 创建所需 topic
            log_info "创建 topic..."
            sleep 3
            docker exec kafka-standalone /opt/kafka/bin/kafka-topics.sh \
                --bootstrap-server kafka-standalone:19092 \
                --create --if-not-exists \
                --topic msg_topic \
                --partitions 10 \
                --replication-factor 1 2>/dev/null || true
            docker exec kafka-standalone /opt/kafka/bin/kafka-topics.sh \
                --bootstrap-server kafka-standalone:19092 \
                --create --if-not-exists \
                --topic offline_msg_topic \
                --partitions 10 \
                --replication-factor 1 2>/dev/null || true
            log_info "Topic 创建完成"
            return 0
        fi
        log_info "等待 Kafka 容器完全启动... (重试 $((retry_count + 1))/6)"
        sleep 5
        retry_count=$((retry_count + 1))
    done

    log_error "单机版 Kafka 启动失败"
    docker-compose -f docker-compose-standalone.yml logs
    exit 1
}

# 启动集群版 Kafka
start_cluster() {
    log_info "启动 3节点 Kafka 集群..."
    cd "$DOCKER_DIR"

    # 检查是否已经运行
    if docker-compose -f docker-compose-cluster.yml ps | grep -c "Up" | grep -q "3"; then
        log_info "3节点 Kafka 集群已经在运行中"
        log_info "集群端点:"
        log_info "  - kafka-1: localhost:29092"
        log_info "  - kafka-2: localhost:39092"
        log_info "  - kafka-3: localhost:49092"
        log_info "Kafka UI: http://localhost:9091"
        return 0
    fi

    # 准备数据目录
    mkdir -p kafka-1/data kafka-2/data kafka-3/data kafka-ui/config
    chown -R 1000:1000 kafka-1/data kafka-2/data kafka-3/data

    docker-compose -f docker-compose-cluster.yml up -d

    log_info "等待 Kafka 集群启动..."
    sleep 10

    local retry_count=0
    while [ $retry_count -lt 6 ]; do
        if docker-compose -f docker-compose-cluster.yml ps | grep -c "Up" | grep -q "3"; then
            log_info "3节点 Kafka 集群启动成功"
            log_info "集群端点:"
            log_info "  - kafka-1: localhost:29092"
            log_info "  - kafka-2: localhost:39092"
            log_info "  - kafka-3: localhost:49092"
            log_info "Kafka UI: http://localhost:9091"

            # 创建所需 topic
            log_info "创建 topic..."
            sleep 3
            docker exec kafka-1 /opt/kafka/bin/kafka-topics.sh \
                --bootstrap-server kafka-1:19092,kafka-2:19092,kafka-3:19092 \
                --create --if-not-exists \
                --topic msg_topic \
                --partitions 10 \
                --replication-factor 3 2>/dev/null || true
            docker exec kafka-1 /opt/kafka/bin/kafka-topics.sh \
                --bootstrap-server kafka-1:19092,kafka-2:19092,kafka-3:19092 \
                --create --if-not-exists \
                --topic offline_msg_topic \
                --partitions 10 \
                --replication-factor 3 2>/dev/null || true
            log_info "Topic 创建完成"
            return 0
        fi
        log_info "等待 Kafka 集群完全启动... (重试 $((retry_count + 1))/6)"
        sleep 5
        retry_count=$((retry_count + 1))
    done

    log_error "3节点 Kafka 集群启动失败"
    docker-compose -f docker-compose-cluster.yml logs
    exit 1
}

# 停止 Kafka
stop_kafka() {
    log_info "停止 Kafka 容器..."
    cd "$DOCKER_DIR"

    docker-compose -f docker-compose-standalone.yml down 2>/dev/null || true
    docker-compose -f docker-compose-cluster.yml down 2>/dev/null || true

    log_info "Kafka 容器已停止"
}

# 重启 Kafka
restart_kafka() {
    log_info "重启 Kafka 容器..."
    stop_kafka
    sleep 2
    start_standalone
}

# 查看状态
show_status() {
    log_info "Kafka 容器状态:"
    echo ""

    # 检查单机版
    if docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | grep -q "kafka-standalone"; then
        log_info "单机版 Kafka:"
        docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | grep "kafka-standalone"
    fi

    # 检查集群版
    if docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | grep -q "kafka-[1-3]"; then
        log_info "集群版 Kafka:"
        docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | grep "kafka-[1-3]"
    fi

    # 检查 Kafka UI
    if docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | grep -q "kafka-ui"; then
        log_info "Kafka UI:"
        docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | grep "kafka-ui"
    fi

    echo ""
    log_info "Kafka 网络:"
    docker network ls | grep kafka || log_warn "未找到 Kafka 网络"
}

# 查看日志
show_logs() {
    log_info "Kafka 容器日志:"
    echo ""

    # 检查单机版
    if docker ps -q -f name=kafka-standalone | grep -q .; then
        log_info "单机版 Kafka 日志:"
        docker logs kafka-standalone --tail 20
        echo ""
    fi

    # 检查集群版
    for i in {1..3}; do
        if docker ps -q -f name=kafka-$i | grep -q .; then
            log_info "kafka-$i 日志:"
            docker logs kafka-$i --tail 10
            echo ""
        fi
    done
}

# 清理容器和数据
clean_kafka() {
    log_warn "这将删除所有 Kafka 容器和数据，确定继续吗? (y/n)"
    read -r response
    if [[ "$response" =~ ^([yY][eE][sS]|[yY])$ ]]; then
        log_info "清理 Kafka 容器和数据..."
        cd "$DOCKER_DIR"

        # 停止并删除容器
        docker-compose -f docker-compose-standalone.yml down -v 2>/dev/null || true
        docker-compose -f docker-compose-cluster.yml down -v 2>/dev/null || true

        # 删除bind mount数据目录
        rm -rf "$DOCKER_DIR"/kafka-standalone/data "$DOCKER_DIR"/kafka-1/data "$DOCKER_DIR"/kafka-2/data "$DOCKER_DIR"/kafka-3/data "$DOCKER_DIR"/kafka-ui/config

        # 删除 Kafka 相关镜像
        docker rmi apache/kafka:4.1.1 2>/dev/null || true
        docker rmi provectuslabs/kafka-ui:master 2>/dev/null || true

        # 删除 Kafka 相关网络
        docker network rm docker_kafka-network 2>/dev/null || true

        log_info "Kafka 容器和数据已清理"
    else
        log_info "取消清理操作"
    fi
}

# 测试 Kafka 连接
test_kafka() {
    log_info "测试 Kafka 连接..."

    # 检查是否有 Kafka 容器运行
    if ! docker ps --format "{{.Names}}" | grep -q "kafka"; then
        log_error "没有运行中的 Kafka 容器"
        return 1
    fi

    # 测试单机版
    if docker ps -q -f name=kafka-standalone | grep -q .; then
        log_info "测试单机版 Kafka..."
        if docker exec kafka-standalone /opt/kafka/bin/kafka-topics.sh \
            --bootstrap-server kafka-standalone:19092 --list &>/dev/null; then
            log_info "单机版 Kafka 连接正常"
            log_info "主题列表:"
            docker exec kafka-standalone /opt/kafka/bin/kafka-topics.sh \
                --bootstrap-server kafka-standalone:19092 --list
        else
            log_error "单机版 Kafka 连接失败"
        fi
    fi

    # 测试集群版
    if docker ps -q -f name=kafka-1 | grep -q .; then
        log_info "测试集群版 Kafka..."
        if docker exec kafka-1 /opt/kafka/bin/kafka-topics.sh \
            --bootstrap-server kafka-1:19092,kafka-2:19092,kafka-3:19092 --list &>/dev/null; then
            log_info "集群版 Kafka 连接正常"
            log_info "主题列表:"
            docker exec kafka-1 /opt/kafka/bin/kafka-topics.sh \
                --bootstrap-server kafka-1:19092,kafka-2:19092,kafka-3:19092 --list
        else
            log_error "集群版 Kafka 连接失败"
        fi
    fi
}

# 创建主题
create_topic() {
    local topic_name="${1:-test-topic}"
    local partitions="${2:-10}"

    log_info "创建主题: $topic_name (分区: $partitions)"

    # 检查是否有 Kafka 容器运行
    if ! docker ps --format "{{.Names}}" | grep -q "kafka"; then
        log_error "没有运行中的 Kafka 容器"
        return 1
    fi

    if docker ps -q -f name=kafka-standalone | grep -q .; then
        docker exec kafka-standalone /opt/kafka/bin/kafka-topics.sh \
            --bootstrap-server kafka-standalone:19092 \
            --create --if-not-exists \
            --topic "$topic_name" \
            --partitions "$partitions" \
            --replication-factor 1
    elif docker ps -q -f name=kafka-1 | grep -q .; then
        docker exec kafka-1 /opt/kafka/bin/kafka-topics.sh \
            --bootstrap-server kafka-1:19092,kafka-2:19092,kafka-3:19092 \
            --create --if-not-exists \
            --topic "$topic_name" \
            --partitions "$partitions" \
            --replication-factor 3
    else
        log_error "无法找到 Kafka 容器"
        return 1
    fi

    log_info "主题 $topic_name 创建成功"
}

# 生产消息
produce_message() {
    local topic_name="${1:-test-topic}"
    local message="${2:-Hello Kafka!}"

    log_info "生产消息到主题: $topic_name"

    if ! docker ps --format "{{.Names}}" | grep -q "kafka"; then
        log_error "没有运行中的 Kafka 容器"
        return 1
    fi

    local bootstrap
    local container
    if docker ps -q -f name=kafka-standalone | grep -q .; then
        bootstrap="kafka-standalone:19092"
        container="kafka-standalone"
    elif docker ps -q -f name=kafka-1 | grep -q .; then
        bootstrap="kafka-1:19092,kafka-2:19092,kafka-3:19092"
        container="kafka-1"
    else
        log_error "无法找到 Kafka 容器"
        return 1
    fi

    echo "$message" | docker exec -i "$container" /opt/kafka/bin/kafka-console-producer.sh \
        --bootstrap-server "$bootstrap" --topic "$topic_name"
    log_info "消息生产成功"
}

# 消费消息
consume_message() {
    local topic_name="${1:-test-topic}"
    local max_messages="${2:-10}"

    log_info "消费消息从主题: $topic_name (最多 $max_messages 条)"

    if ! docker ps --format "{{.Names}}" | grep -q "kafka"; then
        log_error "没有运行中的 Kafka 容器"
        return 1
    fi

    local bootstrap
    local container
    if docker ps -q -f name=kafka-standalone | grep -q .; then
        bootstrap="kafka-standalone:19092"
        container="kafka-standalone"
    elif docker ps -q -f name=kafka-1 | grep -q .; then
        bootstrap="kafka-1:19092,kafka-2:19092,kafka-3:19092"
        container="kafka-1"
    else
        log_error "无法找到 Kafka 容器"
        return 1
    fi

    docker exec "$container" /opt/kafka/bin/kafka-console-consumer.sh \
        --bootstrap-server "$bootstrap" \
        --topic "$topic_name" \
        --from-beginning \
        --max-messages "$max_messages"
    log_info "消息消费完成"
}

# 主函数
main() {
    check_docker

    case "${1:-help}" in
        standalone)
            start_standalone
            ;;
        cluster)
            start_cluster
            ;;
        stop)
            stop_kafka
            ;;
        restart)
            restart_kafka
            ;;
        status)
            show_status
            ;;
        logs)
            show_logs
            ;;
        clean)
            clean_kafka
            ;;
        test)
            test_kafka
            ;;
        create-topic)
            create_topic "${2:-test-topic}" "${3:-10}"
            ;;
        produce)
            produce_message "${2:-test-topic}" "${3:-Hello Kafka!}"
            ;;
        consume)
            consume_message "${2:-test-topic}" "${3:-10}"
            ;;
        help|--help|-h)
            show_help
            ;;
        *)
            log_error "未知命令: $1"
            show_help
            exit 1
            ;;
    esac
}

# 执行主函数
main "$@"
