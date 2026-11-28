#!/usr/bin/env bash
# 起 backbon / auth / group / gw-1 / gw-2（多进程）
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

pkill -9 -f 'build/main' 2>/dev/null
sleep 1
# 清掉上一次 kill -9 留下的 etcd 注册（租约要 300s 才过期，会干扰断言）
etcdctl --endpoints=127.0.0.1:2379 del --prefix /services/ >/dev/null 2>&1 || true
rm -f /tmp/ape-smoke-*.log

SERVICE_INSTANCE_ID=backbon nohup ./build/main --role=backbon >/tmp/ape-smoke-backbon.log 2>&1 &
sleep 2
nohup ./build/main --role=auth >/tmp/ape-smoke-auth.log 2>&1 &
nohup ./build/main --role=group >/tmp/ape-smoke-group.log 2>&1 &
sleep 2
SERVICE_INSTANCE_ID=gw-1 nohup ./build/main --role=gateway --ws-port=6666 --push-port=50055 >/tmp/ape-smoke-gw1.log 2>&1 &
SERVICE_INSTANCE_ID=gw-2 GATEWAY_DRAIN_GRACE_SECONDS=2 \
  nohup ./build/main --role=gateway --ws-port=6667 --push-port=50075 >/tmp/ape-smoke-gw2.log 2>&1 &

for i in $(seq 1 120); do
    if ss -ltn 2>/dev/null | grep -q ':6666 ' && ss -ltn 2>/dev/null | grep -q ':6667 ' && \
       ss -ltn 2>/dev/null | grep -q ':50051 ' && ss -ltn 2>/dev/null | grep -q ':50056 '; then
        echo "services ready"
        exit 0
    fi
    sleep 0.5
done
echo "services failed to start" >&2
tail -n 30 /tmp/ape-smoke-backbon.log /tmp/ape-smoke-auth.log \
    /tmp/ape-smoke-group.log /tmp/ape-smoke-gw1.log /tmp/ape-smoke-gw2.log >&2
exit 1
