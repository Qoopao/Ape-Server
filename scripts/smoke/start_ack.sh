#!/usr/bin/env bash
# 起 ACK 验收测试需要的服务集：backbon + auth + msg + push + gateway-1/2。
# 与 start_services.sh 的区别：
#   1) 包含 msg/push —— ACK（type=106）链路必须经过 PushService.AckConversations；
#   2) 缩短 Outbox 清理间隔，便于验证 docs/ack 的 H 节（保留期清理）用例。
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

pkill -9 -f 'build/main' 2>/dev/null
sleep 1
# 清掉上一次 kill -9 留下的 etcd 注册（租约要 300s 才过期，会干扰断言）
etcdctl --endpoints=127.0.0.1:2379 del --prefix /services/ >/dev/null 2>&1 || true
rm -f /tmp/ape-smoke-*.log

# 保留期清理：默认 10 分钟一轮，这里缩短到 2s 便于测试（生产用默认值）
export OUTBOX_CLEANUP_INTERVAL_MS="${OUTBOX_CLEANUP_INTERVAL_MS:-2000}"

SERVICE_INSTANCE_ID=backbon nohup ./build/main --role=backbon >/tmp/ape-smoke-backbon.log 2>&1 &
sleep 2
nohup ./build/main --role=auth >/tmp/ape-smoke-auth.log 2>&1 &
nohup ./build/main --role=msg >/tmp/ape-smoke-msg.log 2>&1 &
nohup ./build/main --role=push >/tmp/ape-smoke-push.log 2>&1 &
sleep 3
SERVICE_INSTANCE_ID=gw-1 nohup ./build/main --role=gateway --ws-port=6666 --push-port=50055 >/tmp/ape-smoke-gw1.log 2>&1 &
SERVICE_INSTANCE_ID=gw-2 nohup ./build/main --role=gateway --ws-port=6667 --push-port=50075 >/tmp/ape-smoke-gw2.log 2>&1 &

for i in $(seq 1 120); do
    if ss -ltn 2>/dev/null | grep -q ':6666 ' && ss -ltn 2>/dev/null | grep -q ':6667 ' \
       && ss -ltn 2>/dev/null | grep -q ':50053 ' && ss -ltn 2>/dev/null | grep -q ':50054 '; then
        echo "ack services ready"
        sleep 2   # 等 outbox worker / 清理线程就绪
        exit 0
    fi
    sleep 0.5
done
echo "ack services failed to start" >&2
tail -n 30 /tmp/ape-smoke-backbon.log /tmp/ape-smoke-auth.log /tmp/ape-smoke-msg.log \
    /tmp/ape-smoke-push.log /tmp/ape-smoke-gw1.log /tmp/ape-smoke-gw2.log >&2
exit 1
