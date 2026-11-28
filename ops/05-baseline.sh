#!/usr/bin/env bash
source /root/ape-server/ops/env.sh
set +e
echo "=== server ==="
systemctl is-active ape-server.service
ps -o pid,etime,pcpu,rss,args -C main | head -5
echo "=== mysql tables ==="
docker exec mysql mysql -uape_user -pape_password -N -e 'SHOW TABLES;' ape_auth 2>/dev/null
echo "=== mysql counts ==="
docker exec mysql mysql -uape_user -pape_password -N -e 'SELECT "users",COUNT(*) FROM users UNION ALL SELECT "im_outbox",COUNT(*) FROM im_outbox UNION ALL SELECT "im_conversations",COUNT(*) FROM im_conversations UNION ALL SELECT "im_conversation_receipts",COUNT(*) FROM im_conversation_receipts;' ape_auth 2>/dev/null
echo "=== mongo ==="
docker exec ape-mongo mongosh -u root -p root --authenticationDatabase admin IM-System --quiet --eval 'print("msg="+db.msg.countDocuments({})); printjson(db.msg.findOne({},{_id:0,serverMsgID:1,conversationID:1,seq:1}))' 2>&1 | tail -8
echo "=== kafka ==="
docker run --rm --network host apache/kafka:4.1.1 /opt/kafka/bin/kafka-get-offsets.sh --bootstrap-server localhost:29092 --topic msg_topic 2>/dev/null | head -12
echo "=== server errors ==="
journalctl -u ape-server.service -n 300 --no-pager | grep -iE 'error|fail|timeout' | tail -15