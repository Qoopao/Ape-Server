#!/usr/bin/env bash
source /root/ape-server/ops/env.sh
set +e
echo "=== errors ==="
journalctl -u ape-server.service --since "18:42:00" --no-pager | grep -iE "error|PENDING|NOT_FRIEND|DB_ERROR|SendMessages failed|close|kick|busy|timeout" | tail -50
echo "=== counts ==="
journalctl -u ape-server.service --since "18:42:00" --no-pager | grep -c "PushHandler: received"
echo "=== mysql processlist ==="
docker exec mysql mysql -uape_user -pape_password -e 'SHOW PROCESSLIST;' ape_auth 2>/dev/null | head -20
echo "=== outbox states ==="
docker exec mysql mysql -uape_user -pape_password -N -e 'SELECT state,stage,operation,COUNT(*) FROM im_outbox GROUP BY state,stage,operation ORDER BY operation,state;' ape_auth 2>/dev/null