#!/bin/bash

# Docker MySQL 部署管理脚本

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
    echo "Docker MySQL 部署管理脚本"
    echo ""
    echo "用法: $0 [命令] [选项]"
    echo ""
    echo "命令:"
    echo "  standalone    启动单机版 MySQL (开发环境)"
    echo "  stop          停止 MySQL 容器"
    echo "  restart       重启 MySQL 容器"
    echo "  status        查看 MySQL 容器状态"
    echo "  logs          查看 MySQL 日志"
    echo "  clean         清理 MySQL 容器和数据"
    echo "  test          测试 MySQL 连接"
    echo "  backup        备份数据库"
    echo "  restore       恢复数据库"
    echo "  help          显示此帮助信息"
    echo ""
    echo "示例:"
    echo "  $0 standalone          # 启动单机版 MySQL"
    echo "  $0 status              # 查看状态"
    echo "  $0 test                # 测试连接"
    echo "  $0 backup ./backup     # 备份到指定目录"
}

# 检查 Docker 是否安装
check_docker() {
    if ! command -v docker &> /dev/null; then
        log_error "Docker 未安装"
        log_info "请先安装 Docker: https://docs.docker.com/get-docker/"
        exit 1
    fi

    # 检查 Docker Compose
    if command -v docker &> /dev/null && docker compose version &> /dev/null; then
        DOCKER_COMPOSE="docker compose"
    elif command -v docker-compose &> /dev/null; then
        DOCKER_COMPOSE="docker-compose"
    else
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

# 启动单机版 MySQL
start_standalone() {
    log_info "启动单机版 MySQL..."
    cd "$DOCKER_DIR"

    # 检查是否已经运行
    if $DOCKER_COMPOSE -f docker-compose-standalone.yml ps 2>/dev/null | grep -q "Up"; then
        log_info "单机版 MySQL 已经在运行中"
        log_info "客户端端点: mysql://localhost:3306"
        log_info "数据库: ape_auth"
        log_info "用户: ape_user"
        return 0
    fi

    # 准备数据目录
    mkdir -p data/mysql

    $DOCKER_COMPOSE -f docker-compose-standalone.yml up -d

    log_info "等待 MySQL 启动..."
    sleep 5

    local retry_count=0
    while [ $retry_count -lt 10 ]; do
        if $DOCKER_COMPOSE -f docker-compose-standalone.yml ps | grep -q "Up"; then
            # 额外检查 healthcheck 是否通过
            if docker exec mysql mysqladmin ping -h 127.0.0.1 -uape_user -pape_password --silent 2>/dev/null; then
                log_info "单机版 MySQL 启动成功"
                log_info "客户端端点: mysql://localhost:3306"
                log_info "数据库: ape_auth"
                log_info "用户: ape_user"
                return 0
            fi
        fi
        log_info "等待 MySQL 容器完全启动... (重试 $((retry_count + 1))/10)"
        sleep 5
        retry_count=$((retry_count + 1))
    done

    log_error "单机版 MySQL 启动失败"
    $DOCKER_COMPOSE -f docker-compose-standalone.yml logs
    exit 1
}

# 停止 MySQL
stop_mysql() {
    log_info "停止 MySQL 容器..."
    cd "$DOCKER_DIR"

    $DOCKER_COMPOSE -f docker-compose-standalone.yml down 2>/dev/null || true

    log_info "MySQL 容器已停止"
}

# 重启 MySQL
restart_mysql() {
    log_info "重启 MySQL 容器..."
    stop_mysql
    sleep 2
    start_standalone
}

# 查看状态
show_status() {
    log_info "MySQL 容器状态:"
    echo ""

    if docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | grep -q "mysql"; then
        docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | grep "mysql"
    else
        log_warn "没有运行中的 MySQL 容器"
    fi

    echo ""
    log_info "MySQL 网络:"
    docker network ls | grep mysql || log_warn "未找到 MySQL 网络"
}

# 查看日志
show_logs() {
    log_info "MySQL 容器日志:"
    echo ""

    if docker ps -q -f name=^mysql$ | grep -q .; then
        docker logs mysql --tail 50
    else
        log_warn "MySQL 容器未运行"
    fi
}

# 清理容器和数据
clean_mysql() {
    log_warn "这将删除 MySQL 容器和数据，确定继续吗? (y/n)"
    read -r response
    if [[ "$response" =~ ^([yY][eE][sS]|[yY])$ ]]; then
        log_info "清理 MySQL 容器和数据..."
        cd "$DOCKER_DIR"

        # 停止并删除容器
        $DOCKER_COMPOSE -f docker-compose-standalone.yml down -v 2>/dev/null || true

        # 删除bind mount数据目录
        rm -rf "$DOCKER_DIR/data/"

        # 删除 MySQL 相关网络
        docker network rm docker_mysql-network 2>/dev/null || true

        log_info "MySQL 容器和数据已清理"
    else
        log_info "取消清理操作"
    fi
}

# 测试 MySQL 连接
test_mysql() {
    log_info "测试 MySQL 连接..."

    if ! docker ps -q -f name=^mysql$ | grep -q .; then
        log_error "没有运行中的 MySQL 容器"
        return 1
    fi

    if docker exec mysql mysqladmin ping -h 127.0.0.1 -uape_user -pape_password --silent 2>/dev/null; then
        log_info "MySQL 连接正常"
        log_info "数据库列表:"
        docker exec mysql mysql -uape_user -pape_password -e "SHOW DATABASES;" 2>/dev/null
    else
        log_error "MySQL 连接失败"
    fi
}

# 备份数据库
backup_mysql() {
    local backup_dir="${1:-./backup}"

    log_info "备份 MySQL 数据库到: $backup_dir"

    if ! docker ps -q -f name=^mysql$ | grep -q .; then
        log_error "没有运行中的 MySQL 容器"
        return 1
    fi

    mkdir -p "$backup_dir"

    docker exec mysql mysqldump -uape_user -pape_password --all-databases > "$backup_dir/mysql_backup_$(date +%Y%m%d_%H%M%S).sql" 2>/dev/null

    log_info "MySQL 备份完成"
}

# 恢复数据库
restore_mysql() {
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

    log_info "从 $backup_file 恢复 MySQL 数据库..."

    if ! docker ps -q -f name=^mysql$ | grep -q .; then
        log_error "没有运行中的 MySQL 容器"
        return 1
    fi

    docker exec -i mysql mysql -uape_user -pape_password < "$backup_file" 2>/dev/null

    log_info "MySQL 恢复完成"
}

# 主函数
main() {
    # 检查Docker
    check_docker

    # 解析参数
    case "${1:-help}" in
        standalone)
            start_standalone
            ;;
        stop)
            stop_mysql
            ;;
        restart)
            restart_mysql
            ;;
        status)
            show_status
            ;;
        logs)
            show_logs
            ;;
        clean)
            clean_mysql
            ;;
        test)
            test_mysql
            ;;
        backup)
            backup_mysql "${2:-./backup}"
            ;;
        restore)
            restore_mysql "${2}"
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
