#!/usr/bin/env bash
source /root/ape-server/ops/env.sh
set -e
docker compose -f docker/mysql/docker-compose-standalone.yml up -d mysql
docker compose -f docker/kafka/docker-compose-standalone.yml -f ops/ape-kafka.yml up -d kafka-standalone
docker compose -f docker/mongodb/docker-compose-standalone.yml -f ops/ape-mongo.yml up -d mongo
docker compose -f docker/redis/docker-compose-standalone.yml -f ops/ape-redis.yml up -d redis
echo "=== containers ==="
docker ps --format '{{.Names}} {{.Status}}' | grep -E 'mysql|kafka|mongo|redis' || true