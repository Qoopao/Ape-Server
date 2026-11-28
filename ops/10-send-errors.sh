#!/usr/bin/env bash
source /root/ape-server/ops/env.sh
set +e
echo "=== send failures ==="
journalctl -u ape-server.service --since "18:52:20" --no-pager | grep -c "SendMessages failed"
journalctl -u ape-server.service --since "18:52:20" --no-pager | grep -iE "SendMessages failed|deadline|DB_ERROR|error" | tail -20
echo "=== ws errors ==="
journalctl -u ape-server.service --since "18:52:20" --no-pager | grep -ciE "WSSession|error"
echo "=== outbox created ==="
docker exec mysql mysql -uape_user -pape_password -N -e 'SELECT COUNT(*) FROM im_outbox WHERE operation=1 AND created_at >= UNIX_TIMESTAMP("2026-11-28 18:52:20");' ape_auth 2>/dev/null