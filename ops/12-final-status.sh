#!/usr/bin/env bash
source /root/ape-server/ops/env.sh
set +e
echo "=== service ==="
systemctl is-active ape-server.service
curl -sS -m 3 http://127.0.0.1:8080/healthz; echo
curl -sS -m 3 http://127.0.0.1:8080/readyz; echo
echo "=== containers ==="
docker ps --format '{{.Names}} {{.Status}}' | grep -E 'mysql|kafka-standalone|ape-mongo|ape-redis' || true
echo "=== counts ==="
bash /root/ape-server/ops/counts.sh /tmp/ape-final-status.txt
echo "=== server rss ==="
ps -o pid,etime,pcpu,rss,args -C main | head -3