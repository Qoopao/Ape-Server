#!/usr/bin/env bash
source /root/ape-server/ops/env.sh
set +e
sleep 12
docker ps -a --format '{{.Names}} {{.Status}}' | grep -E 'mysql|kafka|mongo|redis|ape-' || true
echo "=== ports ==="
ss -ltn | grep -E ':(3306|6379|27017|29092) ' || true
echo "=== mysql ==="
docker exec mysql mysql -uape_user -pape_password -N -e 'SELECT VERSION(), DATABASE();' ape_auth 2>&1 | head -5
echo "=== redis ==="
docker exec ape-redis redis-cli -a redis123 --no-auth-warning ping 2>&1
echo "=== mongo ==="
docker exec ape-mongo mongosh -u root -p root --authenticationDatabase admin --quiet --eval 'db.runCommand({ping:1}).ok' 2>&1 | tail -3
echo "=== kafka ==="
docker exec kafka-standalone /opt/kafka/bin/kafka-topics.sh --bootstrap-server localhost:9092 --list 2>&1 | head -10