#!/bin/bash

# Docker OpenTelemetry 监控栈部署管理脚本

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
    echo "Docker OpenTelemetry 监控栈部署管理脚本"
    echo ""
    echo "用法: $0 [命令] [选项]"
    echo ""
    echo "命令:"
    echo "  start          启动 OpenTelemetry 监控栈"
    echo "  stop           停止所有容器"
    echo "  restart        重启所有容器"
    echo "  status         查看容器状态"
    echo "  logs           查看容器日志"
    echo "  clean          清理所有容器和数据"
    echo "  test           测试服务连接"
    echo "  help           显示此帮助信息"
    echo ""
    echo "示例:"
    echo "  $0 start                # 启动监控栈"
    echo "  $0 status               # 查看状态"
    echo "  $0 test                 # 测试连接"
}

# 检查 Docker 是否安装
check_docker() {
    if ! command -v docker &> /dev/null; then
        log_error "Docker 未安装"
        log_info "请先安装 Docker: https://docs.docker.com/get-docker/"
        exit 1
    fi

    if command -v docker &> /dev/null && docker compose version &> /dev/null; then
        DOCKER_COMPOSE="docker compose"
    elif command -v docker-compose &> /dev/null; then
        DOCKER_COMPOSE="docker-compose"
    else
        log_error "Docker Compose 未安装"
        log_info "请先安装 Docker Compose: https://docs.docker.com/compose/install/"
        exit 1
    fi

    if ! docker info &> /dev/null; then
        log_error "Docker 服务未运行"
        log_info "请启动 Docker 服务: sudo systemctl start docker"
        exit 1
    fi
}

# 启动 OpenTelemetry 监控栈
start_otel() {
    log_info "启动 OpenTelemetry 监控栈..."
    cd "$DOCKER_DIR"

    if $DOCKER_COMPOSE -f docker-compose.yml ps 2>/dev/null | grep -q "Up"; then
        log_info "OpenTelemetry 监控栈已经在运行中"
        log_info "Grafana:  http://localhost:3000"
        log_info "Jaeger:   http://localhost:16686"
        log_info "Kibana:   http://localhost:5601"
        return 0
    fi

    mkdir -p elasticsearch_data
    chmod -R 777 ./elasticsearch_data

    $DOCKER_COMPOSE -f docker-compose.yml up -d

    log_info "等待 OpenTelemetry 监控栈启动 (需要较长时间)..."
    sleep 5

    local retry_count=0
    while [ $retry_count -lt 12 ]; do
        if docker ps | grep -q "otel-collector"; then
            log_info "OpenTelemetry 监控栈启动成功"
            log_info "OTLP gRPC: localhost:4317"
            log_info "Grafana:   http://localhost:3000"
            log_info "Jaeger:    http://localhost:16686"
            log_info "Kibana:    http://localhost:5601"
            return 0
        fi
        log_info "等待容器完全启动... (重试 $((retry_count + 1))/12)"
        sleep 5
        retry_count=$((retry_count + 1))
    done

    log_error "OpenTelemetry 监控栈启动失败"
    $DOCKER_COMPOSE -f docker-compose.yml logs
    exit 1
}

# 停止 OpenTelemetry 监控栈
stop_otel() {
    log_info "停止 OpenTelemetry 监控栈..."
    cd "$DOCKER_DIR"

    $DOCKER_COMPOSE -f docker-compose.yml down 2>/dev/null || true

    log_info "OpenTelemetry 监控栈已停止"
}

# 重启 OpenTelemetry 监控栈
restart_otel() {
    log_info "重启 OpenTelemetry 监控栈..."
    stop_otel
    sleep 2
    start_otel
}

# 查看状态
show_status() {
    log_info "OpenTelemetry 监控栈容器状态:"
    echo ""

    local containers=("otel-collector" "jaeger-all-in-one" "victoriametrics" "grafana" "elasticsearch" "kibana")
    local has_any=false

    for c in "${containers[@]}"; do
        if docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | grep -q "$c"; then
            has_any=true
        fi
    done

    if $has_any; then
        docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | grep -E "otel-collector|jaeger|victoria|grafana|elasticsearch|kibana"
    else
        log_warn "没有运行中的 OpenTelemetry 容器"
    fi

    echo ""
    log_info "OpenTelemetry 网络:"
    docker network ls | grep otel || log_warn "未找到 otel 网络"
}

# 查看日志
show_logs() {
    local service="${1:-otel-collector}"
    log_info "OpenTelemetry 容器日志 ($service):"
    echo ""

    if docker ps -q -f name="$service" | grep -q .; then
        docker logs "$service" --tail 50
    else
        log_warn "容器 $service 未运行"
    fi
}

# 清理容器和数据
clean_otel() {
    log_warn "这将删除所有 OpenTelemetry 容器和数据，确定继续吗? (y/n)"
    read -r response
    if [[ "$response" =~ ^([yY][eE][sS]|[yY])$ ]]; then
        log_info "清理 OpenTelemetry 容器和数据..."
        cd "$DOCKER_DIR"

        $DOCKER_COMPOSE -f docker-compose.yml down -v 2>/dev/null || true

        rm -rf "$DOCKER_DIR/elasticsearch_data/"

        docker network rm "${PWD##*/}_default" 2>/dev/null || true

        log_info "OpenTelemetry 容器和数据已清理"
    else
        log_info "取消清理操作"
    fi
}

# 测试服务连接
test_otel() {
    log_info "测试 OpenTelemetry 服务连接..."
    echo ""

    if curl -sf http://localhost:3000/api/health &>/dev/null; then
        log_info "Grafana:     http://localhost:3000 正常"
    else
        log_warn "Grafana:     http://localhost:3000 不可用"
    fi

    if curl -sf http://localhost:16686 &>/dev/null; then
        log_info "Jaeger:      http://localhost:16686 正常"
    else
        log_warn "Jaeger:      http://localhost:16686 不可用"
    fi

    if curl -sf http://localhost:9200/_cluster/health &>/dev/null; then
        log_info "Elasticsearch: http://localhost:9200 正常"
    else
        log_warn "Elasticsearch: http://localhost:9200 不可用"
    fi

    if curl -sf http://localhost:5601/api/status &>/dev/null; then
        log_info "Kibana:      http://localhost:5601 正常"
    else
        log_warn "Kibana:      http://localhost:5601 不可用"
    fi
}

# 主函数
main() {
    check_docker

    case "${1:-help}" in
        start)
            start_otel
            ;;
        stop)
            stop_otel
            ;;
        restart)
            restart_otel
            ;;
        status)
            show_status
            ;;
        logs)
            show_logs "${2:-otel-collector}"
            ;;
        clean)
            clean_otel
            ;;
        test)
            test_otel
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

main "$@"
