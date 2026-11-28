#!/usr/bin/env bash
# 群聊 v3 冒烟所需服务集：backbon + auth + group + push + gateway-1/2。
# 与 start_services.sh 的区别：包含 PushService —— 群 251 由 group_topic 的
# GroupFanout 消费后经网关下发，GroupService 自己不再扇出。
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

pkill -9 -f 'build/main' 2>/dev/null
sleep 1
# 清掉上一次 kill -9 留下的 etcd 注册（租约要 300s 才过期，会干扰断言）
etcdctl --endpoints=127.0.0.1:2379 del --prefix /services/ >/dev/null 2>&1 || true
rm -f /tmp/ape-smoke-*.log

# 故障/边界用例需要快速观察 outbox 重试与 tombstone，默认 5s 太长。
export GROUP_PUBLISH_RETRY_MS="${GROUP_PUBLISH_RETRY_MS:-1000}"

SERVICE_INSTANCE_ID=backbon nohup ./build/main --role=backbon >/tmp/ape-smoke-backbon.log 2>&1 &
sleep 2
nohup ./build/main --role=auth >/tmp/ape-smoke-auth.log 2>&1 &
nohup ./build/main --role=group >/tmp/ape-smoke-group.log 2>&1 &
nohup ./build/main --role=push >/tmp/ape-smoke-push.log 2>&1 &
sleep 3
SERVICE_INSTANCE_ID=gw-1 nohup ./build/main --role=gateway --ws-port=6666 --push-port=50055 >/tmp/ape-smoke-gw1.log 2>&1 &
SERVICE_INSTANCE_ID=gw-2 GATEWAY_DRAIN_GRACE_SECONDS=2 \
  nohup ./build/main --role=gateway --ws-port=6667 --push-port=50075 >/tmp/ape-smoke-gw2.log 2>&1 &

for i in $(seq 1 120); do
    if ss -ltn 2>/dev/null | grep -q ':6666 ' && ss -ltn 2>/dev/null | grep -q ':6667 ' \
       && ss -ltn 2>/dev/null | grep -q ':50054 ' && ss -ltn 2>/dev/null | grep -q ':50056 '; then
        echo "group services ready"
        sleep 2   # 等 outbox worker / Kafka consumer 就绪
        exit 0
    fi
    sleep 0.5
done
echo "group services failed to start" >&2
tail -n 30 /tmp/ape-smoke-backbon.log /tmp/ape-smoke-auth.log /tmp/ape-smoke-group.log \
    /tmp/ape-smoke-push.log /tmp/ape-smoke-gw1.log /tmp/ape-smoke-gw2.log >&2
exit 1
