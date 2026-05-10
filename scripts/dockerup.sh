#!/bin/bash

# 检测 Docker 是否可用（可选）
if ! docker info &>/dev/null; then
    echo "Docker 未运行，先启动 Docker Desktop"
    exit 1
fi

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DOCKER_DIR="$SCRIPT_DIR/docker"

echo "=== 启动所有 Docker 服务 ==="

SERVICES=("etcd" "kafka" "mysql" "mongodb" "redis" "otel")

for service in "${SERVICES[@]}"; do
    echo ""
    echo "--- Starting $service ---"
    cd "$DOCKER_DIR/$service"
    bash startup.sh
done

echo ""
echo "=== 所有 Docker 服务启动完毕 ==="
