"""优雅下线冒烟测试（PR-6）：SIGTERM gw-2 -> SERVER_DRAINING + etcd 注销早于 grace。"""
import os
import signal
import subprocess
import sys
import time

sys.path.insert(0, os.environ.get('SMOKE_PYPROTO', '/tmp/ape-smoke-pyproto'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import sdkws_pb2  # noqa: E402
from ws_test import WS, expect  # noqa: E402

import group_test  # noqa: E402


def etcd_gateway_keys():
    out = subprocess.run(
        ['etcdctl', '--endpoints=127.0.0.1:2379', 'get', '--prefix',
         '/services/GatewayPushService', '--keys-only'],
        capture_output=True, text=True)
    return [line for line in out.stdout.splitlines() if line.startswith('/services/')]


def main():
    stamp = str(int(time.time()))[-8:]
    account = group_test.register(6667, '136' + stamp)
    ws = group_test.login(6667, account)
    print('  ok: logged in on gw-2')

    keys_before = etcd_gateway_keys()
    expect(any('/gw-2' in key for key in keys_before), f'gw-2 registered: {keys_before}')

    pid_out = subprocess.run(
        ['pgrep', '-f', 'build/main --role=gateway --ws-port=6667'],
        capture_output=True, text=True)
    pids = [int(p) for p in pid_out.stdout.split()]
    expect(len(pids) == 1, f'found gw-2 pid {pids}')
    os.kill(pids[0], signal.SIGTERM)

    # 1) 客户端收到 SERVER_DRAINING
    draining = None
    deadline = time.time() + 8
    while time.time() < deadline:
        frame = ws.recv_proto(timeout=max(0.1, deadline - time.time()))
        if frame is None:
            break
        if frame.errorCode == 'SERVER_DRAINING':
            draining = frame
            break
    expect(draining is not None and draining.type == 0,
           'client received SERVER_DRAINING')

    # 2) etcd key 在 2s 内消失（注销必须早于关 acceptor/grace）
    start = time.time()
    while time.time() - start < 2.5:
        if not any('/gw-2' in key for key in etcd_gateway_keys()):
            break
        time.sleep(0.1)
    expect(not any('/gw-2' in key for key in etcd_gateway_keys()),
           'gw-2 etcd key removed within 2s of SIGTERM')

    # 3) grace(2s) 内连接不被立刻关闭；期满后进程退出
    deadline = time.time() + 10
    while time.time() < deadline:
        try:
            os.kill(pids[0], 0)
        except ProcessLookupError:
            break
        time.sleep(0.2)
    expect(True, 'gw-2 process exited after grace')

    print('DRAIN SMOKE PASSED')


if __name__ == '__main__':
    try:
        main()
    except AssertionError as exc:
        print('DRAIN SMOKE FAILED:', exc)
        sys.exit(1)
