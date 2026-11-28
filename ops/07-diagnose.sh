#!/usr/bin/env bash
source /root/ape-server/ops/env.sh
set +e
echo "=== server ==="
systemctl is-active ape-server.service
ps -o pid,etime,pcpu,rss,args -C main | head -3
echo "=== recent server log ==="
journalctl -u ape-server.service --since "18:38:00" --no-pager | grep -iE "error|fail|PENDING|outbox|kafka|producer|timeout|disconnect|close" | tail -40
echo "=== outbox ==="
docker exec mysql mysql -uape_user -pape_password -N -e 'SELECT state,stage,COUNT(*) FROM im_outbox WHERE operation=1 GROUP BY state,stage; SELECT "op1_total",COUNT(*) FROM im_outbox WHERE operation=1;' ape_auth 2>/dev/null
echo "=== kafka offsets ==="
docker run --rm --network host apache/kafka:4.1.1 /opt/kafka/bin/kafka-get-offsets.sh --bootstrap-server localhost:29092 --topic msg_topic 2>/dev/null
echo "=== groups ==="
docker run --rm --network host apache/kafka:4.1.1 /opt/kafka/bin/kafka-consumer-groups.sh --bootstrap-server localhost:29092 --describe --group push-service-group 2>/dev/null | head -15
echo "=== mongo count ==="
docker exec ape-mongo mongosh -u root -p root --authenticationDatabase admin IM-System --quiet --eval 'print(db.msg.countDocuments({}))' 2>/dev/null