#!/bin/bash

# Docker MongoDB 部署管理脚本

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
    echo "Docker MongoDB 部署管理脚本"
    echo ""
    echo "用法: $0 [命令] [选项]"
    echo ""
    echo "命令:"
    echo "  standalone    启动单机版 MongoDB (开发环境)"
    echo "  replica       启动3节点副本集 (高可用)"
    echo "  sharded       启动分片集群 (3 shard + 3 config + 1 mongos)"
    echo "  stop          停止所有 MongoDB 容器"
    echo "  restart       重启所有 MongoDB 容器"
    echo "  status        查看 MongoDB 容器状态"
    echo "  logs          查看 MongoDB 日志"
    echo "  clean         清理所有 MongoDB 容器和数据"
    echo "  test          测试 MongoDB 连接"
    echo "  help          显示此帮助信息"
    echo ""
    echo "选项:"
    echo "  -f, --file    指定 docker-compose 文件"
    echo "  -d, --detach  后台运行"
    echo ""
    echo "示例:"
    echo "  $0 standalone          # 启动单机版 MongoDB"
    echo "  $0 replica             # 启动3节点副本集"
    echo "  $0 sharded             # 启动分片集群"
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

# 启动单机版 MongoDB
start_standalone() {
    log_info "启动单机版 MongoDB..."
    cd "$DOCKER_DIR"

    # 检查是否已经运行
    if $DOCKER_COMPOSE -f docker-compose-standalone.yml ps 2>/dev/null | grep -q "Up"; then
        log_info "单机版 MongoDB 已经在运行中"
        log_info "客户端端点: mongodb://localhost:27017"
        log_info "Mongo Express: http://localhost:8081"
        return 0
    fi

    # 准备数据目录
    mkdir -p data/mongodb

    $DOCKER_COMPOSE -f docker-compose-standalone.yml up -d

    log_info "等待 MongoDB 启动..."
    sleep 3

    local retry_count=0
    while [ $retry_count -lt 6 ]; do
        if $DOCKER_COMPOSE -f docker-compose-standalone.yml ps | grep -q "Up"; then
            log_info "单机版 MongoDB 启动成功"
            log_info "客户端端点: mongodb://localhost:27017"
            log_info "Mongo Express: http://localhost:8081"
            return 0
        fi
        log_info "等待 MongoDB 容器完全启动... (重试 $((retry_count + 1))/6)"
        sleep 3
        retry_count=$((retry_count + 1))
    done

    log_error "单机版 MongoDB 启动失败"
    $DOCKER_COMPOSE -f docker-compose-standalone.yml logs
    exit 1
}

# 启动副本集 MongoDB
start_replica() {
    log_info "启动 3节点 MongoDB 副本集..."
    cd "$DOCKER_DIR"

    # 检查是否已经运行
    if $DOCKER_COMPOSE -f docker-compose-replica.yml ps 2>/dev/null | grep -c "Up" | grep -q "3"; then
        log_info "3节点 MongoDB 副本集已经在运行中"
        log_info "副本集端点:"
        log_info "  - mongodb-1: localhost:27018"
        log_info "  - mongodb-2: localhost:27019"
        log_info "  - mongodb-3: localhost:27020"
        log_info "Mongo Express: http://localhost:8081"
        return 0
    fi

    # 准备数据目录
    mkdir -p data/mongodb-1 data/mongodb-2 data/mongodb-3

    $DOCKER_COMPOSE -f docker-compose-replica.yml up -d

    log_info "等待 MongoDB 副本集启动..."
    sleep 5

    local retry_count=0
    while [ $retry_count -lt 10 ]; do
        if $DOCKER_COMPOSE -f docker-compose-replica.yml ps | grep -c "Up" | grep -q "3"; then
            log_info "3节点 MongoDB 副本集启动成功"
            log_info "副本集端点:"
            log_info "  - mongodb-1: localhost:27018"
            log_info "  - mongodb-2: localhost:27019"
            log_info "  - mongodb-3: localhost:27020"
            log_info "Mongo Express: http://localhost:8081"
            return 0
        fi
        log_info "等待副本集完全启动... (重试 $((retry_count + 1))/10)"
        sleep 5
        retry_count=$((retry_count + 1))
    done

    log_error "3节点 MongoDB 副本集启动失败"
    $DOCKER_COMPOSE -f docker-compose-replica.yml logs
    exit 1
}

# 启动分片集群 MongoDB
start_shard() {
    log_info "启动 MongoDB 分片集群 (3 shard + 3 config + 1 mongos)..."
    cd "$DOCKER_DIR"

    # 检查是否已经运行
    if $DOCKER_COMPOSE -f docker-compose-sharded.yml ps 2>/dev/null | grep -c "Up" | grep -q "8"; then
        log_info "MongoDB 分片集群已经在运行中"
        log_info "mongos 端点: mongodb://localhost:27021"
        log_info "Shard 节点:"
        log_info "  - mongodb-1: localhost:27018"
        log_info "  - mongodb-2: localhost:27019"
        log_info "  - mongodb-3: localhost:27020"
        log_info "Config 节点:"
        log_info "  - mongodb-config-1: localhost:27022"
        log_info "  - mongodb-config-2: localhost:27023"
        log_info "  - mongodb-config-3: localhost:27024"
        log_info "Mongo Express: http://localhost:8082"
        return 0
    fi

    # 准备数据目录
    mkdir -p data/mongodb-1 data/mongodb-2 data/mongodb-3
    mkdir -p data/mongodb_config_1 data/mongodb_config_2 data/mongodb_config_3

    $DOCKER_COMPOSE -f docker-compose-sharded.yml up -d

    log_info "等待分片集群启动 (需要较长时间，约60秒)..."
    sleep 10

    local retry_count=0
    while [ $retry_count -lt 12 ]; do
        if $DOCKER_COMPOSE -f docker-compose-sharded.yml ps | grep -c "Up" | grep -q "8"; then
            log_info "MongoDB 分片集群所有容器已启动"
            log_info "mongos 端点: mongodb://localhost:27021"
            log_info "Shard 节点:"
            log_info "  - mongodb-1: localhost:27018"
            log_info "  - mongodb-2: localhost:27019"
            log_info "  - mongodb-3: localhost:27020"
            log_info "Config 节点:"
            log_info "  - mongodb-config-1: localhost:27022"
            log_info "  - mongodb-config-2: localhost:27023"
            log_info "  - mongodb-config-3: localhost:27024"
            log_info "Mongo Express: http://localhost:8082"
            return 0
        fi
        log_info "等待分片集群完全启动... (重试 $((retry_count + 1))/12)"
        sleep 5
        retry_count=$((retry_count + 1))
    done

    log_error "MongoDB 分片集群启动失败"
    $DOCKER_COMPOSE -f docker-compose-sharded.yml logs
    exit 1
}

# 停止 MongoDB
stop_mongodb() {
    log_info "停止 MongoDB 容器..."
    cd "$DOCKER_DIR"

    $DOCKER_COMPOSE -f docker-compose-standalone.yml down 2>/dev/null || true
    $DOCKER_COMPOSE -f docker-compose-replica.yml down 2>/dev/null || true
    $DOCKER_COMPOSE -f docker-compose-sharded.yml down 2>/dev/null || true

    log_info "MongoDB 容器已停止"
}

# 重启 MongoDB 
restart_mongodb() {
    log_info "重启 MongoDB 容器..."
    stop_mongodb
    sleep 2
    start_standalone
}

# 查看状态
show_status() {
    log_info "MongoDB 容器状态:"
    echo ""

    # 检查单机版
    if docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | grep -q "mongo"; then
        log_info "单机版 MongoDB:"
        docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | grep "mongo"
    fi

    # 检查副本集
    if docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | grep -q "mongodb-[1-3]"; then
        log_info "副本集 MongoDB:"
        docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | grep "mongodb-[1-3]"
    fi

    # 检查分片集群 config server
    if docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | grep -q "mongodb-config"; then
        log_info "分片集群 Config 节点:"
        docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | grep "mongodb-config"
    fi

    # 检查 mongos
    if docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | grep -q "mongos"; then
        log_info "分片集群 mongos:"
        docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | grep "mongos"
    fi

    # 检查 Mongo Express
    if docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | grep -q "mongo-express\|ape-mongo-express"; then
        log_info "Mongo Express:"
        docker ps --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}" | grep "mongo-express\|ape-mongo-express"
    fi

    echo ""
    log_info "MongoDB 网络:"
    docker network ls | grep mongo || log_warn "未找到 MongoDB 网络"
}

# 查看日志
show_logs() {
    log_info "MongoDB 容器日志:"
    echo ""

    # 单机版
    if docker ps -q -f name=^mongo$ | grep -q .; then
        log_info "单机版 MongoDB 日志:"
        docker logs mongo --tail 20
        echo ""
    fi

    # 副本集 / 分片 shard 节点
    for i in {1..3}; do
        if docker ps -q -f name=mongodb-$i\$ | grep -q .; then
            log_info "mongodb-$i 日志:"
            docker logs mongodb-$i --tail 10
            echo ""
        fi
    done

    # 分片 config 节点
    for i in {1..3}; do
        if docker ps -q -f name=mongodb-config-$i\$ | grep -q .; then
            log_info "mongodb-config-$i 日志:"
            docker logs mongodb-config-$i --tail 10
            echo ""
        fi
    done

    # mongos
    if docker ps -q -f name=^mongos$ | grep -q .; then
        log_info "mongos 日志:"
        docker logs mongos --tail 20
        echo ""
    fi
}

# 清理容器和数据
clean_mongodb() {
    log_warn "这将删除所有 MongoDB 容器和数据，确定继续吗? (y/n)"
    read -r response
    if [[ "$response" =~ ^([yY][eE][sS]|[yY])$ ]]; then
        log_info "清理 MongoDB 容器和数据..."
        cd "$DOCKER_DIR"

        # 停止并删除容器
        $DOCKER_COMPOSE -f docker-compose-standalone.yml down -v 2>/dev/null || true
        $DOCKER_COMPOSE -f docker-compose-replica.yml down -v 2>/dev/null || true
        $DOCKER_COMPOSE -f docker-compose-sharded.yml down -v 2>/dev/null || true

        # 删除bind mount数据目录
        rm -rf "$DOCKER_DIR/data/"

        # 删除 MongoDB 相关网络
        docker network rm docker_mongo-network 2>/dev/null || true

        log_info "MongoDB 容器和数据已清理"
    else
        log_info "取消清理操作"
    fi
}

# 测试 MongoDB 连接
test_mongodb() {
    log_info "测试 MongoDB 连接..."

    if ! docker ps --format "{{.Names}}" | grep -q "mongo"; then
        log_error "没有运行中的 MongoDB 容器"
        return 1
    fi

    # 测试单机版
    if docker ps -q -f name=^mongo$ | grep -q .; then
        log_info "测试单机版 MongoDB..."
        if docker exec mongo mongosh -u root -p root --authenticationDatabase admin --eval "db.runCommand({ping:1})" --quiet 2>/dev/null; then
            log_info "单机版 MongoDB 连接正常"
        else
            log_error "单机版 MongoDB 连接失败"
        fi
    fi

    # 测试副本集
    if docker ps -q -f name=mongodb-1 | grep -q .; then
        log_info "测试副本集 MongoDB..."
        if docker exec mongodb-1 mongosh -u root -p root --authenticationDatabase admin --eval "rs.status().ok" --quiet 2>/dev/null; then
            log_info "副本集 MongoDB 连接正常"
        else
            log_error "副本集 MongoDB 连接失败"
        fi
    fi

    # 测试 mongos (分片集群)
    if docker ps -q -f name=^mongos$ | grep -q .; then
        log_info "测试分片集群 mongos..."
        if docker exec mongos mongosh -u root -p root --authenticationDatabase admin --eval "sh.status().ok" --quiet 2>/dev/null; then
            log_info "分片集群 mongos 连接正常"
        else
            log_error "分片集群 mongos 连接失败"
        fi
    fi
}

# 初始化数据库
init_mongodb() {
    log_info "初始化 MongoDB 数据库..."
    
    # 检查是否有 MongoDB 容器运行
    if ! docker ps --format "{{.Names}}" | grep -q "mongodb"; then
        log_error "没有运行中的 MongoDB 容器"
        return 1
    fi
    
    # 初始化单机版
    if docker ps -q -f name=mongodb-standalone | grep -q .; then
        log_info "初始化单机版 MongoDB..."
        docker exec mongodb-standalone mongosh --eval "$(cat mongo-init.js)"
        log_info "单机版 MongoDB 初始化完成"
    fi
    
    # 初始化副本集
    if docker ps -q -f name=mongodb-1 | grep -q .; then
        log_info "初始化副本集 MongoDB..."
        docker exec mongodb-1 mongosh --eval "$(cat replica-init.js)"
        log_info "副本集 MongoDB 初始化完成"
    fi
}

# 备份数据库
backup_mongodb() {
    local backup_dir="${1:-./backup}"
    
    log_info "备份 MongoDB 数据库到: $backup_dir"
    
    # 创建备份目录
    mkdir -p "$backup_dir"
    
    # 备份单机版
    if docker ps -q -f name=mongodb-standalone | grep -q .; then
        log_info "备份单机版 MongoDB..."
        docker exec mongodb-standalone mongodump --out /tmp/backup
        docker cp mongodb-standalone:/tmp/backup "$backup_dir/standalone"
        docker exec mongodb-standalone rm -rf /tmp/backup
    fi
    
    # 备份副本集
    if docker ps -q -f name=mongodb-1 | grep -q .; then
        log_info "备份副本集 MongoDB..."
        docker exec mongodb-1 mongodump --out /tmp/backup
        docker cp mongodb-1:/tmp/backup "$backup_dir/replica"
        docker exec mongodb-1 rm -rf /tmp/backup
    fi
    
    log_info "MongoDB 备份完成"
}

# 恢复数据库
restore_mongodb() {
    local backup_dir="${1:-./backup}"
    
    log_info "从 $backup_dir 恢复 MongoDB 数据库"
    
    if [ ! -d "$backup_dir" ]; then
        log_error "备份目录不存在: $backup_dir"
        return 1
    fi
    
    # 恢复单机版
    if [ -d "$backup_dir/standalone" ] && docker ps -q -f name=mongodb-standalone | grep -q .; then
        log_info "恢复单机版 MongoDB..."
        docker cp "$backup_dir/standalone" mongodb-standalone:/tmp/backup
        docker exec mongodb-standalone mongorestore /tmp/backup
        docker exec mongodb-standalone rm -rf /tmp/backup
    fi
    
    # 恢复副本集
    if [ -d "$backup_dir/replica" ] && docker ps -q -f name=mongodb-1 | grep -q .; then
        log_info "恢复副本集 MongoDB..."
        docker cp "$backup_dir/replica" mongodb-1:/tmp/backup
        docker exec mongodb-1 mongorestore /tmp/backup
        docker exec mongodb-1 rm -rf /tmp/backup
    fi
    
    log_info "MongoDB 恢复完成"
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
        replica)
            start_replica
            ;;
        shard)
            start_shard
            ;;
        stop)
            stop_mongodb
            ;;
        restart)
            restart_mongodb
            ;;
        status)
            show_status
            ;;
        logs)
            show_logs
            ;;
        clean)
            clean_mongodb
            ;;
        test)
            test_mongodb
            ;;
        init)
            init_mongodb
            ;;
        backup)
            backup_mongodb "${2:-./backup}"
            ;;
        restore)
            restore_mongodb "${2:-./backup}"
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
