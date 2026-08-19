#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DOCKER_DIR="$SCRIPT_DIR/docker"
BUILD_DIR="$SCRIPT_DIR/build"

echo "停止 etcd..."
cd "$DOCKER_DIR/etcd" && ./deploy.sh stop 2>/dev/null || true

echo "停止 kafka..."
cd "$DOCKER_DIR/kafka" && ./deploy.sh stop 2>/dev/null || true

echo "停止 redis..."
cd "$DOCKER_DIR/redis" && ./deploy.sh stop 2>/dev/null || true

echo "停止 mongodb..."
cd "$DOCKER_DIR/mongodb" && ./deploy.sh stop 2>/dev/null || true

echo "停止 mysql..."
cd "$DOCKER_DIR/mysql" && ./deploy.sh stop 2>/dev/null || true

echo "停止 OpenTelemetry..."
cd "$DOCKER_DIR/otel" && ./deploy.sh stop 2>/dev/null || true

echo "所有服务已停止"

cd "$BUILD_DIR" && make -j4