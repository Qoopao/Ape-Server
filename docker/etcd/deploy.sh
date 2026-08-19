#!/bin/bash

# Docker etcd 部署管理脚本

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
    echo "Docker etcd 部署管理脚本"
    echo ""
    echo "用法: $0 [命令] [选项]"
    echo ""
    echo "命令:"
    echo "  standalone    启动单机版etcd (开发环境)"
    echo "  cluster       启动3节点etcd集群 (生产环境)"
    echo "  stop          停止所有etcd容器"
    echo "  restart       重启所有etcd容器"
    echo "  status        查看etcd容器状态"
    echo "  logs          查看etcd日志"
    echo "  clean         清理所有etcd容器和数据"
    echo "  test          测试etcd连接"
    echo "  help          显示此帮助信息"
    echo ""
    echo "选项:"
    echo "  -f, --file    指定docker-compose文件"
    echo "  -d, --detach  后台运行"
    echo ""
    echo "示例:"
    echo "  $0 standalone          # 启动单机版etcd"
    echo "  $0 cluster             # 启动3节点集群"
    echo "  $0 status              # 查看状态"
    echo "  $0 test                # 测试连接"
}

# 检查Docker是否安装
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

    # 检查Docker服务是否运行
    if ! docker info &> /dev/null; then
        log_error "Docker 服务未运行"
        log_info "请启动 Docker 服务: sudo systemctl start docker"
        exit 1
    fi
}

# 启动单机版etcd
start_standalone() {
    log_info "启动单机版 etcd..."
    cd "$DOCKER_DIR"
    
    # 检查是否已经运行
    if docker-compose -f docker-compose-standalone.yml ps | grep -q "Up"; then
        log_info "单机版 etcd 已经在运行中"
        log_info "客户端端点: http://localhost:2379"
        return 0
    fi
    
    # 使用 Docker Compose 命令
    docker-compose -f docker-compose-standalone.yml up -d
    
    log_info "等待 etcd 启动..."
    sleep 2
    
    # 验证启动
    # 等待容器启动
    local retry_count=0
    while [ $retry_count -lt 3 ]; do
        if docker-compose -f docker-compose-standalone.yml ps | grep -q "Up"; then
            log_info "单机版 etcd 启动成功"
            log_info "客户端端点: http://localhost:2379"
            return 0
        fi
        log_info "等待 etcd 容器完全启动... (重试 $((retry_count + 1))/3)"
        sleep 3
        retry_count=$((retry_count + 1))
    done
    
    log_error "单机版 etcd 启动失败"
    docker-compose -f docker-compose-standalone.yml logs
    exit 1
}

# 启动集群版etcd
start_cluster() {
    log_info "启动 3节点 etcd 集群..."
    cd "$DOCKER_DIR"
    
    # 检查是否已经运行
    if docker-compose -f docker-compose-cluster.yml ps | grep -c "Up" | grep -q "3"; then
        log_info "3节点 etcd 集群已经在运行中"
        log_info "集群端点:"
        log_info "  - etcd-1: http://localhost:2379"
        log_info "  - etcd-2: http://localhost:2381"
        log_info "  - etcd-3: http://localhost:2383"
        return 0
    fi
    
    # 使用 Docker Compose 命令
    docker-compose -f docker-compose-cluster.yml up -d
    
    log_info "等待 etcd 集群启动..."
    sleep 5
    
    # 验证启动
    # 等待所有容器启动
    local retry_count=0
    while [ $retry_count -lt 3 ]; do
        if docker-compose -f docker-compose-cluster.yml ps | grep -c "Up" | grep -q "3"; then
            log_info "3节点 etcd 集群启动成功"
            log_info "集群端点:"
            log_info "  - etcd-1: http://localhost:2379"
            log_info "  - etcd-2: http://localhost:2381"
            log_info "  - etcd-3: http://localhost:2383"
            return 0
        fi
        log_info "等待 etcd 集群完全启动... (重试 $((retry_count + 1))/3)"
        sleep 5
        retry_count=$((retry_count + 1))
    done
    
    log_error "3节点 etcd 集群启动失败"
    docker-compose -f docker-compose-cluster.yml logs
    exit 1
}

# 停止etcd
stop_etcd() {
    log_info "停止 etcd 容器..."
    cd "$DOCKER_DIR"
    
    # 停止所有etcd相关容器
    docker-compose -f docker-compose-standalone.yml down 2>/dev/null || true
    docker-compose -f docker-compose-cluster.yml down 2>/dev/null || true
    
    log_info "etcd 容器已停止"
}

# 重启etcd
restart_etcd() {
    log_info "重启 etcd 容器..."
    stop_etcd
    sleep 2
    start_standalone
}

# 查看状态
show_status() {
    log_info "etcd 容器状态:"
    echo ""
    
    # 检查单机版
    if docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | grep -q "etcd-standalone"; then
        log_info "单机版 etcd:"
        docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | grep "etcd-standalone"
    fi
    
    # 检查集群版
    if docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | grep -q "etcd-[1-3]"; then
        log_info "集群版 etcd:"
        docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | grep "etcd-[1-3]"
    fi
    
    echo ""
    log_info "etcd 网络:"
    docker network ls | grep etcd || log_warn "未找到 etcd 网络"
}

# 查看日志
show_logs() {
    log_info "etcd 容器日志:"
    echo ""
    
    # 检查单机版
    if docker ps -q -f name=etcd-standalone | grep -q .; then
        log_info "单机版 etcd 日志:"
        docker logs etcd-standalone --tail 20
        echo ""
    fi
    
    # 检查集群版
    for i in {1..3}; do
        if docker ps -q -f name=etcd-$i | grep -q .; then
            log_info "etcd-$i 日志:"
            docker logs etcd-$i --tail 10
            echo ""
        fi
    done
}

# 清理容器和数据
clean_etcd() {
    log_warn "这将删除所有 etcd 容器和数据，确定继续吗? (y/n)"
    read -r response
    if [[ "$response" =~ ^([yY][eE][sS]|[yY])$ ]]; then
        log_info "清理 etcd 容器和数据..."
        cd "$DOCKER_DIR"
        
        # 停止并删除容器
        docker-compose -f docker-compose-standalone.yml down -v 2>/dev/null || true
        docker-compose -f docker-compose-cluster.yml down -v 2>/dev/null || true
        
        # 删除bind mount数据目录
        rm -rf "$DOCKER_DIR"/etcd_standalone-data "$DOCKER_DIR"/etcd_data_1 "$DOCKER_DIR"/etcd_data_2 "$DOCKER_DIR"/etcd_data_3

        # 删除etcd相关镜像
        docker rmi quay.io/coreos/etcd:v3.5.10 2>/dev/null || true

        # 删除etcd相关网络
        docker network rm docker_etcd-network 2>/dev/null || true

        log_info "etcd 容器和数据已清理"
    else
        log_info "取消清理操作"
    fi
}

# 测试etcd连接
test_etcd() {
    log_info "测试 etcd 连接..."
    
    # 检查是否有etcd容器运行
    if ! docker ps --format "{{.Names}}" | grep -q "etcd"; then
        log_error "没有运行中的 etcd 容器"
        return 1
    fi
    
    # 测试单机版
    if docker ps -q -f name=etcd-standalone | grep -q .; then
        log_info "测试单机版 etcd..."
        if docker exec etcd-standalone etcdctl endpoint health; then
            log_info "单机版 etcd 连接正常"
        else
            log_error "单机版 etcd 连接失败"
        fi
    fi
    
    # 测试集群版
    if docker ps -q -f name=etcd-1 | grep -q .; then
        log_info "测试集群版 etcd..."
        for i in {1..3}; do
            if docker exec etcd-$i etcdctl endpoint health; then
                log_info "etcd-$i 连接正常"
            else
                log_error "etcd-$i 连接失败"
            fi
        done
        
        # 显示集群信息
        log_info "集群信息:"
        docker exec etcd-1 etcdctl member list
    fi
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
        cluster)
            start_cluster
            ;;
        stop)
            stop_etcd
            ;;
        restart)
            restart_etcd
            ;;
        status)
            show_status
            ;;
        logs)
            show_logs
            ;;
        clean)
            clean_etcd
            ;;
        test)
            test_etcd
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