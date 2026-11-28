#!/usr/bin/env bash
# 单进程兼容模式：停掉拆分服务，起 --role=all
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

pkill -9 -f 'build/main' 2>/dev/null
sleep 1
etcdctl --endpoints=127.0.0.1:2379 del --prefix /services/ >/dev/null 2>&1 || true
rm -f /tmp/ape-smoke-all.log
nohup ./build/main --role=all >/tmp/ape-smoke-all.log 2>&1 &
for i in $(seq 1 120); do
  if ss -ltn 2>/dev/null | grep -q ':6666 ' && ss -ltn 2>/dev/null | grep -q ':50052 '; then
    echo "role=all ready"
    exit 0
  fi
  sleep 0.5
done
echo "role=all failed to start" >&2
grep -vE 'OTLP|MeterContext' /tmp/ape-smoke-all.log | tail -n 30 >&2
exit 1
