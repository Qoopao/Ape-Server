#!/usr/bin/env bash
source /root/ape-server/ops/env.sh
set +e
OUT="${1:-/tmp/ape-counts.txt}"
{
  echo "timestamp=$(date -Is)"
  docker exec mysql mysql -uape_user -pape_password -N -B -e 'SELECT CONCAT("mysql_users=",COUNT(*)) FROM users; SELECT CONCAT("outbox_op1=",COUNT(*)) FROM im_outbox WHERE operation=1; SELECT CONCAT("outbox_pending=",COUNT(*)) FROM im_outbox WHERE operation=1 AND state IN (0,1);' ape_auth 2>/dev/null
  docker exec ape-mongo mongosh -u root -p root --authenticationDatabase admin IM-System --quiet --eval 'print("mongo_msg="+db.msg.countDocuments({}))' 2>/dev/null
  SUM=$(docker run --rm --network host apache/kafka:4.1.1 /opt/kafka/bin/kafka-get-offsets.sh --bootstrap-server localhost:29092 --topic msg_topic 2>/dev/null | awk -F: '{s+=$3} END {print s+0}')
  echo "kafka_msg_topic_offsets=$SUM"
} > "$OUT"
cat "$OUT"