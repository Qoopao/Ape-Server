#!/bin/bash

# Docker Redis 部署管理脚本

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
    echo "Docker Redis 部署管理脚本"
    echo ""
    echo "用法: $0 [命令] [选项]"
    echo ""
    echo "命令:"
    echo "  standalone    启动单机版 Redis (开发环境)"
    echo "  stop          停止 Redis 容器"
    echo "  restart       重启 Redis 容器"
    echo "  status        查看 Redis 容器状态"
    echo "  logs          查看 Redis 日志"
    echo "  clean         清理 Redis 容器和数据"
    echo "  test          测试 Redis 连接"
    echo "  backup        备份 Redis 数据"
    echo "  restore       恢复 Redis 数据"
    echo "  help          显示此帮助信息"
    echo ""
    echo "示例:"
    echo "  $0 standalone          # 启动单机版 Redis + Redis Commander"
    echo "  $0 status              # 查看状态"
    echo "  $0 test                # 测试连接"
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

# 启动单机版 Redis
start_standalone() {
    log_info "启动单机版 Redis..."
    cd "$DOCKER_DIR"

    if $DOCKER_COMPOSE -f docker-compose-standalone.yml ps 2>/dev/null | grep -q "Up"; then
        log_info "单机版 Redis 已经在运行中"
        log_info "Redis 端点:    redis://localhost:6379 (密码: redis123)"
        log_info "Redis Web UI:  http://localhost:8088 (admin/admin123)"
        return 0
    fi

    mkdir -p data/redis

    $DOCKER_COMPOSE -f docker-compose-standalone.yml up -d

    log_info "等待 Redis 启动..."
    sleep 2

    local retry_count=0
    while [ $retry_count -lt 10 ]; do
        if docker exec redis redis-cli -a redis123 ping 2>/dev/null | grep -q "PONG"; then
            log_info "单机版 Redis 启动成功"
            log_info "Redis 端点:    redis://localhost:6379 (密码: redis123)"
            log_info "Redis Web UI:  http://localhost:8088 (admin/admin123)"
            return 0
        fi
        log_info "等待 Redis 容器完全启动... (重试 $((retry_count + 1))/10)"
        sleep 2
        retry_count=$((retry_count + 1))
    done

    log_error "单机版 Redis 启动失败"
    $DOCKER_COMPOSE -f docker-compose-standalone.yml logs
    exit 1
}

# 停止 Redis
stop_redis() {
    log_info "停止 Redis 容器..."
    cd "$DOCKER_DIR"

    $DOCKER_COMPOSE -f docker-compose-standalone.yml down 2>/dev/null || true

    log_info "Redis 容器已停止"
}

# 重启 Redis
restart_redis() {
    log_info "重启 Redis 容器..."
    stop_redis
    sleep 2
    start_standalone
}

# 查看状态
show_status() {
    log_info "Redis 容器状态:"
    echo ""

    if docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | grep -qE "redis|redis-commander"; then
        docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | grep -E "redis|redis-commander"
    else
        log_warn "没有运行中的 Redis 容器"
    fi

    echo ""
    log_info "Redis 网络:"
    docker network ls | grep redis || log_warn "未找到 Redis 网络"

    echo ""
    log_info "Redis 内存使用:"
    if docker ps -q -f name=^redis$ | grep -q .; then
        docker exec redis redis-cli -a redis123 INFO memory 2>/dev/null | grep -E "used_memory_human|used_memory_peak_human" || true
    fi
}

# 查看日志
show_logs() {
    local service="${1:-redis}"
    log_info "Redis 容器日志 ($service):"
    echo ""

    if docker ps -q -f name="^${service}$" | grep -q .; then
        docker logs "$service" --tail 50
    else
        log_warn "容器 $service 未运行"
    fi
}

# 清理容器和数据
clean_redis() {
    log_warn "这将删除 Redis 容器和数据，确定继续吗? (y/n)"
    read -r response
    if [[ "$response" =~ ^([yY][eE][sS]|[yY])$ ]]; then
        log_info "清理 Redis 容器和数据..."
        cd "$DOCKER_DIR"

        $DOCKER_COMPOSE -f docker-compose-standalone.yml down -v 2>/dev/null || true

        rm -rf "$DOCKER_DIR/data/"

        docker network rm docker_redis-network 2>/dev/null || true

        log_info "Redis 容器和数据已清理"
    else
        log_info "取消清理操作"
    fi
}

# 测试 Redis 连接
test_redis() {
    log_info "测试 Redis 连接..."

    if ! docker ps -q -f name=^redis$ | grep -q .; then
        log_error "没有运行中的 Redis 容器"
        return 1
    fi

    if docker exec redis redis-cli -a redis123 ping 2>/dev/null | grep -q "PONG"; then
        log_info "Redis 连接正常"
        log_info "Redis 信息:"
        docker exec redis redis-cli -a redis123 INFO server 2>/dev/null | grep -E "redis_version|redis_mode|os|uptime_in_days" || true
    else
        log_error "Redis 连接失败"
    fi
}

# 备份 Redis 数据
backup_redis() {
    local backup_dir="${1:-./backup}"

    log_info "备份 Redis 数据到: $backup_dir"

    if ! docker ps -q -f name=^redis$ | grep -q .; then
        log_error "没有运行中的 Redis 容器"
        return 1
    fi

    mkdir -p "$backup_dir"

    docker exec redis redis-cli -a redis123 --rdb /data/dump.rdb SAVE 2>/dev/null
    docker cp redis:/data/dump.rdb "$backup_dir/redis_backup_$(date +%Y%m%d_%H%M%S).rdb" 2>/dev/null

    log_info "Redis 备份完成"
}

# 恢复 Redis 数据
restore_redis() {
    local backup_file="${1}"

    if [ -z "$backup_file" ]; then
        log_error "请指定备份文件路径"
        echo "用法: $0 restore <backup_file>"
        return 1
    fi

    if [ ! -f "$backup_file" ]; then
        log_error "备份文件不存在: $backup_file"
        return 1
    fi

    log_info "从 $backup_file 恢复 Redis 数据..."

    if ! docker ps -q -f name=^redis$ | grep -q .; then
        log_error "没有运行中的 Redis 容器"
        return 1
    fi

    docker cp "$backup_file" redis:/data/dump.rdb 2>/dev/null
    docker exec redis redis-cli -a redis123 SHUTDOWN NOSAVE 2>/dev/null || true
    sleep 2
    start_standalone

    log_info "Redis 恢复完成"
}

# 主函数
main() {
    check_docker

    case "${1:-help}" in
        standalone)
            start_standalone
            ;;
        stop)
            stop_redis
            ;;
        restart)
            restart_redis
            ;;
        status)
            show_status
            ;;
        logs)
            show_logs "${2:-redis}"
            ;;
        clean)
            clean_redis
            ;;
        test)
            test_redis
            ;;
        backup)
            backup_redis "${2:-./backup}"
            ;;
        restore)
            restore_redis "${2}"
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
