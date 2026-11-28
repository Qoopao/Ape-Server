#!/usr/bin/env bash
source /root/ape-server/ops/env.sh
set +e
systemctl daemon-reload
systemctl reset-failed ape-server.service 2>/dev/null || true
systemctl enable --now ape-server.service
for i in $(seq 1 90); do
  if ss -ltn | grep -q ':6666 ' && ss -ltn | grep -q ':50058 '; then
    echo "ape-server ports ready after ${i}s"
    break
  fi
  sleep 1
done
sleep 2
systemctl is-active ape-server.service
ss -ltn | grep -E ':(6666|5005[1-8]|8080) ' || true
echo "=== health ==="
curl -sS -m 3 http://127.0.0.1:8080/healthz || true
echo
curl -sS -m 3 http://127.0.0.1:8080/readyz || true
echo
echo "=== log tail ==="
journalctl -u ape-server.service -n 40 --no-pager | tail -40