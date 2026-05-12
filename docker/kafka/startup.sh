#!/bin/bash
# 确保数据目录存在且属主为容器内 appuser (uid=1000, gid=1000)
mkdir -p kafka-1/data kafka-2/data kafka-3/data kafka-ui/config
chown -R 1000:1000 kafka-1/data kafka-2/data kafka-3/data
docker-compose up -d

# 等待 Kafka 完全就绪（最多等 30 秒）
echo "Waiting for Kafka to be ready..."
for i in $(seq 1 30); do
  if docker exec kafka-1 /opt/kafka/bin/kafka-topics.sh \
       --bootstrap-server kafka-1:19092 --list &>/dev/null; then
    break
  fi
  sleep 2
done

# 创建所需 topic（若已存在则忽略报错）
docker exec kafka-1 /opt/kafka/bin/kafka-topics.sh \
  --bootstrap-server kafka-1:19092,kafka-2:19092,kafka-3:19092 \
  --create --if-not-exists \
  --topic msg_topic \
  --partitions 10 \
  --replication-factor 3

docker exec kafka-1 /opt/kafka/bin/kafka-topics.sh \
  --bootstrap-server kafka-1:19092,kafka-2:19092,kafka-3:19092 \
  --create --if-not-exists \
  --topic offline_msg_topic \
  --partitions 10 \
  --replication-factor 3

echo "Kafka topics ready."
